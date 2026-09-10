//! Just enough HTTP/1.1 to find the response head, read the few fields a
//! caller needs from it, and hand the body onward.
//!
//! Deliberately small. It does *not* understand chunked transfer-encoding: S3
//! answers a ranged GET with `206` and a `Content-Length`, so nothing here has
//! needed it yet. Chunked is rejected rather than guessed at -- guessing would
//! silently truncate a transfer instead of failing one.

use alloc::string::String;
use alloc::vec::Vec;

/// Where a response body goes.
///
/// The benchmark only wants the count, which the parser keeps regardless, so
/// it sinks into [`NullSink`]. A caller that wants the bytes -- DuckDB reading
/// a range into a page -- uses [`BufferSink`] or its own implementation.
pub trait BodySink {
    fn write(&mut self, data: &[u8]);

    /// Bytes the sink actually stored, less than the body if it discarded
    /// some or ran out of room. A trait method, not something the caller
    /// reads off its own sink afterwards, because [`crate::Conn`] owns the
    /// sink once installed and `dyn BodySink` cannot be downcast back.
    fn written(&self) -> u64 {
        0
    }

    /// The sink was handed more than it could store.
    fn overflowed(&self) -> bool {
        false
    }
}

/// Counts and discards. A ZST, so boxing one does not allocate.
pub struct NullSink;

impl BodySink for NullSink {
    fn write(&mut self, _data: &[u8]) {}
}

/// Writes the body straight into a caller-owned buffer.
///
/// Holds a raw pointer rather than a slice: a lifetime here would put that
/// lifetime on [`crate::Conn`] and on the worker that owns it. The caller
/// keeps the buffer alive and unaliased until the request reports done --
/// that contract is why the constructor is unsafe.
pub struct BufferSink {
    buf: *mut u8,
    cap: usize,
    written: usize,
    overflowed: bool,
}

// SAFETY: the buffer is handed to exactly one connection, which is polled by
// exactly one worker thread.
unsafe impl Send for BufferSink {}

impl BufferSink {
    /// # Safety
    ///
    /// `buf` must point to `cap` writable bytes that stay valid, and stay
    /// unread by anyone else, until the connection using this sink finishes.
    pub unsafe fn new(buf: *mut u8, cap: usize) -> Self {
        Self {
            buf,
            cap,
            written: 0,
            overflowed: false,
        }
    }

    pub fn written(&self) -> usize {
        self.written
    }

    /// True if the peer sent more than the buffer could hold. The excess is
    /// dropped, not written past the end -- but the response is then not the
    /// one that was asked for, and the caller has to treat it as a failure
    /// rather than a short read.
    pub fn overflowed(&self) -> bool {
        self.overflowed
    }
}

impl BodySink for BufferSink {
    fn written(&self) -> u64 {
        self.written as u64
    }

    fn overflowed(&self) -> bool {
        self.overflowed
    }

    fn write(&mut self, data: &[u8]) {
        let room = self.cap - self.written;
        let n = core::cmp::min(room, data.len());
        if n < data.len() {
            self.overflowed = true;
        }
        if n > 0 {
            // SAFETY: n <= cap - written, and the constructor's contract makes
            // buf..buf+cap writable for as long as this sink lives.
            unsafe { core::ptr::copy_nonoverlapping(data.as_ptr(), self.buf.add(self.written), n) };
            self.written += n;
        }
    }
}

/// `Content-Range: bytes 0-65535/10737418240`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ContentRange {
    pub first: u64,
    pub last: u64,
    /// `None` for the `*` form, where the server declines to say.
    pub total: Option<u64>,
}

impl ContentRange {
    pub fn len(&self) -> u64 {
        self.last.saturating_sub(self.first) + 1
    }

    pub fn is_empty(&self) -> bool {
        self.last < self.first
    }
}

/// What the response head said. Only the fields something above this layer
/// reads: httpfs needs these four to size a file, validate a range and detect
/// that an object changed under it.
#[derive(Debug, Default, Clone)]
pub struct ResponseHead {
    pub status: u16,
    pub content_length: Option<u64>,
    pub content_range: Option<ContentRange>,
    pub etag: Option<String>,
    pub last_modified: Option<String>,
}

/// A head this large is a peer doing something wrong, not a large response.
/// Bounded because the head is buffered to be parsed as a whole.
const MAX_HEAD: usize = 64 * 1024;

pub(crate) struct ResponseParser {
    pending: Vec<u8>,
    headers_done: bool,
    /// How much of the CRLFCRLF terminator has been seen. Carried across calls
    /// because it can straddle two TLS records.
    hdr_state: u8,
    head: ResponseHead,
    body_bytes: u64,
}

/// Why a response could not be read. Returned rather than logged so the
/// connection fails visibly instead of delivering a short body.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ParseError {
    /// No status line, or one that is not `HTTP/1.x NNN`.
    BadStatusLine,
    /// The head grew past `MAX_HEAD` without terminating.
    HeadTooLong,
    /// `Transfer-Encoding: chunked`. Not implemented; see the module comment.
    Chunked,
}

impl ResponseParser {
    pub(crate) fn new() -> Self {
        Self {
            pending: Vec::new(),
            headers_done: false,
            hdr_state: 0,
            head: ResponseHead::default(),
            body_bytes: 0,
        }
    }

    pub(crate) fn status(&self) -> u16 {
        self.head.status
    }

    pub(crate) fn head(&self) -> &ResponseHead {
        &self.head
    }

    /// Body bytes only. The head is application data as far as TLS is
    /// concerned, so counting raw decrypted length overcounts by one head per
    /// connection and makes a byte-exact check impossible.
    pub(crate) fn body_bytes(&self) -> u64 {
        self.body_bytes
    }

    pub(crate) fn headers_done(&self) -> bool {
        self.headers_done
    }

    pub(crate) fn feed(&mut self, data: &[u8], sink: &mut dyn BodySink) -> Result<(), ParseError> {
        if self.headers_done {
            self.body_bytes += data.len() as u64;
            sink.write(data);
            return Ok(());
        }
        for (i, &b) in data.iter().enumerate() {
            self.hdr_state = match (self.hdr_state, b) {
                (0, b'\r') => 1,
                (1, b'\n') => 2,
                (2, b'\r') => 3,
                (3, b'\n') => 4,
                (_, b'\r') => 1,
                _ => 0,
            };
            if self.hdr_state == 4 {
                self.pending.extend_from_slice(&data[..=i]);
                self.parse_head()?;
                self.headers_done = true;
                self.pending = Vec::new();
                let body = &data[i + 1..];
                self.body_bytes += body.len() as u64;
                sink.write(body);
                return Ok(());
            }
        }
        if self.pending.len() + data.len() > MAX_HEAD {
            return Err(ParseError::HeadTooLong);
        }
        self.pending.extend_from_slice(data);
        Ok(())
    }

    /// Count bytes without looking at them. Used when the record layer is
    /// stubbed out: the payload is ciphertext, so there is no head to find and
    /// no body to separate.
    pub(crate) fn count_opaque(&mut self, n: usize) {
        self.body_bytes += n as u64;
    }

    fn parse_head(&mut self) -> Result<(), ParseError> {
        let mut lines = self.pending.split(|&b| b == b'\n');

        let status_line = lines.next().ok_or(ParseError::BadStatusLine)?;
        self.head.status = parse_status(status_line).ok_or(ParseError::BadStatusLine)?;

        for line in lines {
            let line = trim(line);
            if line.is_empty() {
                continue;
            }
            let (name, value) = match split_header(line) {
                Some(pair) => pair,
                None => continue, // a continuation or a malformed line; ignore
            };
            if eq_ignore_case(name, b"content-length") {
                self.head.content_length = parse_u64(value);
            } else if eq_ignore_case(name, b"content-range") {
                self.head.content_range = parse_content_range(value);
            } else if eq_ignore_case(name, b"etag") {
                self.head.etag = to_string(value);
            } else if eq_ignore_case(name, b"last-modified") {
                self.head.last_modified = to_string(value);
            } else if eq_ignore_case(name, b"transfer-encoding")
                && contains_ignore_case(value, b"chunked")
            {
                return Err(ParseError::Chunked);
            }
        }
        Ok(())
    }
}

/// `HTTP/1.1 206 Partial Content` -> 206.
fn parse_status(line: &[u8]) -> Option<u16> {
    let line = trim(line);
    if !line.starts_with(b"HTTP/") {
        return None;
    }
    let rest = line.split(|&b| b == b' ').nth(1)?;
    if rest.len() != 3 {
        return None;
    }
    let mut code = 0u16;
    for &b in rest {
        if !b.is_ascii_digit() {
            return None;
        }
        code = code * 10 + (b - b'0') as u16;
    }
    Some(code)
}

/// `bytes 0-65535/10737418240`, or `bytes 0-65535/*`.
fn parse_content_range(value: &[u8]) -> Option<ContentRange> {
    let value = trim(value);
    let rest = value.strip_prefix(b"bytes ")?;
    let (range, total) = split_once(rest, b'/')?;
    let (first, last) = split_once(range, b'-')?;
    Some(ContentRange {
        first: parse_u64(first)?,
        last: parse_u64(last)?,
        total: if trim(total) == b"*" {
            None
        } else {
            Some(parse_u64(total)?)
        },
    })
}

fn split_header(line: &[u8]) -> Option<(&[u8], &[u8])> {
    let (name, value) = split_once(line, b':')?;
    Some((trim(name), trim(value)))
}

fn split_once(s: &[u8], sep: u8) -> Option<(&[u8], &[u8])> {
    let i = s.iter().position(|&b| b == sep)?;
    Some((&s[..i], &s[i + 1..]))
}

fn trim(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|b| !b.is_ascii_whitespace()).unwrap_or(s.len());
    let end = s
        .iter()
        .rposition(|b| !b.is_ascii_whitespace())
        .map_or(start, |i| i + 1);
    &s[start..end]
}

fn parse_u64(s: &[u8]) -> Option<u64> {
    let s = trim(s);
    if s.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &b in s {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u64)?;
    }
    Some(v)
}

fn eq_ignore_case(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len()
        && a.iter()
            .zip(b)
            .all(|(x, y)| x.to_ascii_lowercase() == y.to_ascii_lowercase())
}

fn contains_ignore_case(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || haystack.len() < needle.len() {
        return false;
    }
    haystack
        .windows(needle.len())
        .any(|w| eq_ignore_case(w, needle))
}

/// Header values are ASCII in practice; anything else is not a value we read.
fn to_string(value: &[u8]) -> Option<String> {
    core::str::from_utf8(value).ok().map(String::from)
}

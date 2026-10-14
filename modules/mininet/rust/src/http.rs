//! Just enough HTTP/1.1 to find the response head and hand the body on.
//! Chunked transfer-encoding is rejected, not guessed at.

use alloc::string::String;
use alloc::vec::Vec;

/// Where a response body goes.
pub trait BodySink {
    fn write(&mut self, data: &[u8]);

    /// Bytes the sink stored.
    fn written(&self) -> u64 {
        0
    }

    /// The sink was handed more than it could store.
    fn overflowed(&self) -> bool {
        false
    }
}

/// Writes the body into a caller-owned buffer that outlives the request.
pub struct BufferSink {
    buf: *mut u8,
    cap: usize,
    written: usize,
    overflowed: bool,
}

unsafe impl Send for BufferSink {}

impl BufferSink {
    /// # Safety
    /// `buf` points to `cap` writable bytes nobody else touches until the request finishes.
    pub unsafe fn new(buf: *mut u8, cap: usize) -> Self {
        Self {
            buf,
            cap,
            written: 0,
            overflowed: false,
        }
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

/// The head fields something above this layer reads.
#[derive(Debug, Default, Clone)]
pub struct ResponseHead {
    pub status: u16,
    pub content_length: Option<u64>,
    pub content_range: Option<ContentRange>,
    pub etag: Option<String>,
    pub last_modified: Option<String>,
}

/// Bounded because the head is buffered whole.
const MAX_HEAD: usize = 64 * 1024;

pub(crate) struct ResponseParser {
    pending: Vec<u8>,
    headers_done: bool,
    /// CRLFCRLF progress; it can straddle two TLS records.
    hdr_state: u8,
    head: ResponseHead,
    body_bytes: u64,
}

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

    pub(crate) fn head_mut(&mut self) -> &mut ResponseHead {
        &mut self.head
    }

    /// Body bytes only.
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

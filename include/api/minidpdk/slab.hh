#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include <minidpdk/util.hh>
#include <minidpdk/stack.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>
#include <osv/types.h>

namespace minidpdk {
class mem_pool;

using rte_mbuf_extbuf_free_callback_t = void (*)(void *addr, void *opaque);

struct rte_mbuf_ext_shared_info {
  void *fcb_opaque;
  rte_mbuf_extbuf_free_callback_t free_cb;
  uint16_t refcnt;
};

struct alignas(64) mbuf {
  // next in chain
  mbuf *next;

  // address of the mbuf structure
  char *buf_addr;

  // memory pool
  mem_pool *pool;

  // IO Virtual Address
  uintptr_t iova;

  // offset of the dataroom
  uint16_t data_offset;

  // size of the packet(chained)
  // size of the used fraction of the dataroom
  // total length of the buffer
  uint16_t pkt_len, data_len, buf_len;

  // reference count
  uint16_t refcnt;

  // length of the l2 header
  // length of the l3 header
  // length of the l4 header
  // number of segments in the chain
  uint16_t l2_len : 5;
  uint16_t l3_len : 6;
  uint16_t l4_len : 5;
  uint16_t nb_segs;

  // rss hash
  struct {
    uint64_t rss;
  } hash;

  // NIC offload flags
  uint64_t ol_flags;

  rte_mbuf_ext_shared_info *shinfo;

  // type of the packet
  uint32_t packet_type;

  mbuf() = default;
  mbuf(mbuf *next, mem_pool *sb, uintptr_t iova, uint32_t size,
       uint16_t nb_segs, uint16_t data_len, uint16_t headroom)
      : next(next), buf_addr(reinterpret_cast<char *>(this) + sizeof(mbuf)),
        pool(sb), iova(iova + sizeof(mbuf) + headroom), data_offset(headroom),
        pkt_len(), data_len(data_len), buf_len(size), refcnt(1),
        nb_segs(nb_segs), ol_flags(), shinfo(nullptr) {}

  uint8_t *buf_start() { return reinterpret_cast<uint8_t *>(buf_addr); }

  template <typename T> T *data(size_t offset = 0) {
    return reinterpret_cast<T *>(buf_start() + data_offset + offset);
  }

  const void *read(uint32_t off, uint32_t len, void *buf) {
    if (off + len <= data_len)
      return data<uint8_t *>(off);

    auto *seg = this;
    while (seg && off >= seg->data_len) {
      off -= seg->data_len;
      seg = seg->next;
    }

    uint32_t copied = 0;
    while (seg && copied < len) {
      auto *src = seg->data<uint8_t>() + off;
      auto n = std::min<uint32_t>(seg->data_len - off, len - copied);
      std::memcpy(static_cast<uint8_t *>(buf) + copied, src, n);
      copied += n;
      off = 0;
      seg = seg->next;
    }

    return copied == len ? buf : nullptr;
  }

  mbuf *last_seg() {
    auto *seg = this;
    while (seg->next)
      seg = seg->next;
    return seg;
  }

  template <typename T> T *prepend() {
    data_offset -= sizeof(T);
    data_len += sizeof(T);
    iova -= sizeof(T);
    return data<T>();
  }

  void adj(uint16_t len) {
    data_offset += len;
    data_len -= len;
    iova += len;
  }
};

struct alignas(64) obj_header {
  obj_header *next;
  uintptr_t iova;
};

inline void mbuf_free(mbuf *buf);

struct alignas(64) page_header {
  page_header *next;
  page_header *prev;
  uintptr_t iova;

  static void list_remove(page_header *s) {
    s->prev->next = s->next;
    s->next->prev = s->prev;
  }

  page_header() : next(nullptr), prev(nullptr) {}
};

static_assert(sizeof(page_header) % 64 == 0, "");

inline void mbuf_free(mbuf *buf);

using init_fn_t = void (*)(mbuf **, uint16_t, void *);
struct page_storage {
  static constexpr size_t kDefaultCacheSize = 256;
  struct page_list {
    page_header head, tail;
    page_list() : head(), tail() {
      head.next = &tail;
      tail.prev = &head;
    }

    void list_push(page_header *s) {
      s->next = head.next;
      s->prev = &head;
      head.next->prev = s;
      head.next = s;
    }

    bool empty() const { return head.next == &tail; }

    page_header *front() { return head.next; }
  };

  page_list regions;
  page_storage() : regions() {}
};

inline void mbuf_free(mbuf *buf);
using mbuf_ptr = std::unique_ptr<mbuf, decltype(&mbuf_free)>;

class mem_pool {
public:
  static constexpr size_t kDefaultHeadroom = 128;
  // EC2's jumbo MTU is 9001, so the largest frame is 9015. Rounded up so that
  // kMaxDataLen + kDefaultHeadroom stays a multiple of 64 and kDefaultSize
  // keeps its cache-line alignment.
  static constexpr size_t kMaxDataLen = 9216;
  static constexpr size_t kDefaultSize =
      kMaxDataLen + kDefaultHeadroom + sizeof(mbuf) + sizeof(obj_header);
  static_assert(kDefaultSize % 64  == 0, "");
  static constexpr size_t kSlabSize = 2 * 1024 * 1024;

public:
  mem_pool(unsigned size, void *priv = nullptr, init_fn_t init_fn = nullptr)
      : ps(), objs(stack::create(size)), obj_size(kDefaultSize), top(size), priv(priv), init_fn(init_fn){
    while (top > 0)
      alloc_new_region();
  }

  mbuf *alloc_default() {
    void* obj;  
    if(!objs->pop(reinterpret_cast<void**>(&obj), 1))
        return nullptr;
    return static_cast<mbuf*>(obj);

  }

  uintptr_t get_iova(void * ptr){
      auto *ph = reinterpret_cast<page_header*>(reinterpret_cast<uintptr_t>(ptr) & ~(kSlabSize - 1));
      auto *ptr_byte = static_cast<uint8_t*>(ptr);
      auto *ph_byte = reinterpret_cast<uint8_t*>(ph);
      return ph->iova + (ptr_byte - ph_byte);
  }

  int alloc_bulk(void **pkts, unsigned n) { 
    return objs->pop(pkts, n);
  }

  mbuf *alloc_single() { return alloc_default(); }

  void alloc_new_region() {
    auto pa = mem::frames::alloc(kSlabSize, kSlabSize);
    assert(pa != mem::frames::no_memory);
    auto *region = mem::map_phys(pa, kSlabSize);
    auto *s = new (region) page_header();
    auto *base = reinterpret_cast<uint8_t *>(region) + sizeof(page_header);
    s->iova = pa;
    size_t space = kSlabSize - sizeof(page_header);
    ps.regions.list_push(s);
    size_t off = 0;
    while (top > 0 && off + obj_size <= space) {
      auto *obj = new (base + off) obj_header;
      obj->next = nullptr;
      obj->iova = s->iova + sizeof(page_header) + off;
      auto *m = new (obj + 1) mbuf(nullptr, this,
                         obj->iova + sizeof(obj_header), kMaxDataLen, 1, 0,
                         kDefaultHeadroom);
      assert(m->iova == get_iova(m) + sizeof(mbuf) + kDefaultHeadroom);
      objs->push(reinterpret_cast<void* const*>(&m), 1);
      off += obj_size;
      top--;
    }
  }

  __inline void free_single_mbuf(mbuf *obj) {
    auto *obj_hdr = reinterpret_cast<obj_header *>(
        reinterpret_cast<uint8_t *>(obj) - sizeof(obj_header));
    new (obj) mbuf(nullptr, this,
                   obj_hdr->iova + sizeof(obj_header), kMaxDataLen, 1, 0,
                   kDefaultHeadroom);
      assert(obj->iova == get_iova(obj) + sizeof(mbuf) + kDefaultHeadroom);
      objs->push(reinterpret_cast<void* const*>(&obj), 1);
  }

  void free_mbuf(mbuf *obj) {
    assert(obj->refcnt == 0);
    auto *obj_ptr = obj;
    while (obj_ptr) {
      auto *next = obj_ptr->next;
      free_single_mbuf(obj_ptr);
      obj_ptr = next;
    }
  }

  constexpr size_t get_data_size() const { return kMaxDataLen; }

  mbuf_ptr alloc_default_safe() {
    auto *pkt = alloc_default();
    return mbuf_ptr(pkt, mbuf_free);
  }

  ~mem_pool() {
    auto free_pages = [](page_storage::page_list &list) {
      auto *s = list.head.next;
      while (s != &list.tail) {
        auto *next = s->next;
        mem::frames::free(s->iova, kSlabSize);
        s = next;
      }
    };
    free_pages(ps.regions);
    stack::destroy(objs);
  }

private:
  page_storage ps;
  stack *objs;
  size_t obj_size;
  size_t top = 0;

public:
  void *priv;
  init_fn_t init_fn;
};

inline mbuf *alloc_mbuf(mem_pool *sb) {
  return sb->alloc_default();
}

inline void mbuf_free(mbuf *buf) {
  assert(buf->pool);
  assert(buf->refcnt >= 1);
  --buf->refcnt;
  if (buf->refcnt)
    return;
  buf->pool->free_mbuf(buf);
}

inline mbuf_ptr mbuf_take_owner_ship(mbuf *pkt) {
  return mbuf_ptr(pkt, &mbuf_free);
}
} // namespace minidpdk

#pragma once

#ifndef _NODE_MSGPACK_UNPACK_H_
#define _NODE_MSGPACK_UNPACK_H_

#include <assert.h>

#include <deque>
#include <stack>

#include <node.h>
#include <node_buffer.h>

#include "endian.h"
#include "nan.h"

using namespace v8;
using namespace node;

namespace node_msgpack {

union msgpack_part_union {
  uint8_t  u8;
  uint16_t u16;
  uint32_t u32;
  uint64_t u64;
  int8_t   i8;
  int16_t  i16;
  int32_t  i32;
  int64_t  i64;
  float    d32;
  double   d64;
};

struct msgpack_part {

  inline uint8_t type() const { return type_.u; }

  template <uint8_t Mask>
  inline uint8_t fix() const {
    return type_.u & Mask;
  };
  inline int8_t fix() const {
    return type_.i;
  };

  template <typename T, uint8_t Mask>
  inline T value() const;
  template <typename T>
  inline T value() const;

  inline const msgpack_part_union &as() const {
    return *(msgpack_part_union *)(((char *)this) + 1);
  };

  const union {
    uint8_t u;
     int8_t i;
  } type_;
};

template <>
inline uint8_t msgpack_part::fix<0xff>() const {
  return type_.u;
}

template<>
inline uint8_t msgpack_part::value() const {
  return as().u8;
}

template<>
inline uint16_t msgpack_part::value() const {
  return ntohs(as().u16);
}

template<>
inline uint32_t msgpack_part::value() const {
  return ntohl(as().u32);
}

template<>
inline uint64_t msgpack_part::value() const {
  return be64toh(as().u64);
}

template<>
inline int8_t msgpack_part::value() const {
  return as().i8;
}

template<>
inline int16_t msgpack_part::value() const {
  return value<uint16_t>();
}

template<>
inline int32_t msgpack_part::value() const {
  return value<uint32_t>();
}

template<>
inline int64_t msgpack_part::value() const {
  return value<uint64_t>();
}

template<>
inline float msgpack_part::value() const {
  uint32_t i = value<uint32_t>();
  return reinterpret_cast<float &>(i);
}

template<>
inline double msgpack_part::value() const {
  uint64_t i = value<uint64_t>();
  return reinterpret_cast<double &>(i);
}

class msgpack_part_reader {
public:
  msgpack_part_reader(char *head, size_t size)
  : part_(NULL), head_(head), size_(size), offset_(0) {
  }

  inline bool next() {
    if      (size_ <  offset_) throw bad_data();
    else if (size_ == offset_) return false;
    part_ = reinterpret_cast<msgpack_part *>(const_cast<char *>(head_) + offset_);
    offset_ += sizeof(part_->type_);
    return true;
  }

public:
  inline uint8_t type() const { return part_->type(); }

  template <uint8_t Mask>
  inline uint8_t fix() const { return part_->fix<Mask>(); }
  inline int8_t fix() const { return part_->fix(); }

  template <typename T>
  inline T value() { offset_ += sizeof(T); return part_->value<T>(); }

  template <typename T>
  inline const char *data() {
    length_ = value<T>();
    const char *p = head_ + offset_;
    offset_ += length_;
    return p;
  }

  template <uint8_t Mask>
  inline const char *data() {
    length_ = fix<Mask>();
    const char *p = head_ + offset_;
    offset_ += length_;
    return p;
  }

public:
  inline size_t   size() const { return size_; }
  inline size_t offset() const { return offset_; }
  inline size_t length() const { return length_; }

private:
  msgpack_part *part_;
  const char  *head_;
  const size_t size_;
  size_t length_;
  size_t offset_;
};

struct unpak {
  unpak(Local<Value> o, uint32_t l) : v(o), i(0) {
    is_array = v->IsArray();
    len = is_array ? l : l * 2;
  }

  inline bool is_value() {
    return i & 1;
  }

  inline Local<Array> array() {
    assert(v->IsArray());
    return v.As<Array>();
  }
  inline Local<Object> object() {
    assert(v->IsObject());
    return v->ToObject();
  }

  Local<Value> v;
  Local<Value> key;
  uint32_t i;
  uint32_t len;
  bool is_array;
};

inline Local<Value> Unpack(Local<Value> val) {
  NanScope();

  assert(Buffer::HasInstance(val));
  Local<Object> buf = val->ToObject();

  size_t len = Buffer::Length(buf);
  if (len == 0) return Local<Value>::New(Undefined());

  msgpack_part_reader r(Buffer::Data(buf), len);
  std::stack<unpak> stack;
  Local<Value> result;

  while (r.next()) {
    Local<Value> v;
    uint32_t l = 0;

    switch (r.type()) {
      case 0xc0: // nil
        v = Local<Value>::New(Null());
        break;

      case 0xc2: // false
        v = Local<Value>::New(False());
        break;

      case 0xc3: // true
        v = Local<Value>::New(True());
        break;

      case 0xca: // float 32
        v = Number::New(r.value<float>());
        break;

      case 0xcb: // float 64
        v = Number::New(r.value<double>());
        break;

      case 0x00 ... 0x7f: // positive fixint
        v = Integer::New(r.fix<0xff>());
        break;

      case 0xcc: // uint 8
        v = Integer::NewFromUnsigned(r.value<uint8_t>());
        break;

      case 0xcd: // uint 16
        v = Integer::NewFromUnsigned(r.value<uint16_t>());
        break;

      case 0xce: // uint 32
        v = Integer::NewFromUnsigned(r.value<uint32_t>());
        break;

      case 0xcf: // uint 64
        v = Number::New(r.value<uint64_t>());
        break;

      case 0xe0 ... 0xff: // negative fixint
        v = Integer::New(r.fix());
        break;

      case 0xd0: // int 8
        v = Integer::New(r.value<int8_t>());
        break;

      case 0xd1: // int 16
        v = Integer::New(r.value<int16_t>());
        break;

      case 0xd2: // int 32
        v = Integer::New(r.value<int32_t>());
        break;

      case 0xd3: // int 64
        v = Number::New(r.value<int64_t>());
        break;

      case 0xa0 ... 0xbf: // fixstr
        v = String::New(r.data<0x1f>(), r.length());
        break;

      case 0xd9: // str 8
        v = String::New(r.data<uint8_t>(), r.length());
        break;

      case 0xda: // str 16
        v = String::New(r.data<uint16_t>(), r.length());
        break;

      case 0xdb: // str 32
        v = String::New(r.data<uint32_t>(), r.length());
        break;

      case 0x90 ... 0x9f: // fixarray
        l = r.fix<0x0f>();
        v = Array::New(l);
        break;

      case 0xdc: // array 16
        l = r.value<uint16_t>();
        v = Array::New(l);
        break;

      case 0xdd: // array 32
        l = r.value<uint32_t>();
        v = Array::New(l);
        break;

      case 0x80 ... 0x8f: // fixmap
        l = r.fix<0x0f>();
        v = Object::New();
        break;

      case 0xde: // map 16
        l = r.value<uint16_t>();
        v = Object::New();
        break;

      case 0xdf: // map 32
        l = r.value<uint32_t>();
        v = Object::New();
        break;

      case 0xc4: // bin 8
      case 0xc5: // bin 16
      case 0xc6: // bin 32
      case 0xc7: // ext 8
      case 0xc8: // ext 16
      case 0xc9: // ext 32

      case 0xd4: // fixext 1
      case 0xd5: // fixext 2
      case 0xd6: // fixext 4
      case 0xd7: // fixext 8
      case 0xd8: // fixext 16

      default:
        throw type_error();
    }

    assert(!v.IsEmpty());

    if (stack.empty()) {
      if (0 < l) {
        stack.push(unpak(v, l));
      } else {
        result = v;
        break;
      }
    } else {
      unpak &parent = stack.top();
      if (parent.is_array) {
        if (parent.i < parent.len) {
          parent.array()->Set(parent.i, v);
        }
      } else {
        if (parent.i < parent.len) {
          if (parent.is_value()) {
            assert(!parent.key.IsEmpty());
            parent.object()->Set(parent.key, v);
          } else {
            parent.key = v;
          }
        }
      }
      parent.i++;
      if (0 < l) {
        stack.push(unpak(v, l));
      } else if (parent.len <= parent.i) {
        stack.pop();
        if (stack.empty()) {
          result = parent.v;
          break;
        }
      }
    }
  }

  if (!stack.empty()) result = stack.top().v;
  assert(!result.IsEmpty());

  buf->Set(NanSymbol("offset"), Integer::New(r.offset()));

  return result;
}

} // namespace node_msgpack

#endif

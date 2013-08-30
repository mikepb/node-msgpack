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

class Unpacker {
public:
  Unpacker(const char *head, size_t size)
  : head_(head), size_(size), length_(0), offset_(0) {}

private:
  union as_union {
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

  struct part {
    inline uint8_t type() const {
      return type_;
    }

    template <uint8_t Mask>
    inline uint8_t fix() const {
      return type_ & Mask;
    };
    inline int8_t fix() const {
      return *(int8_t *)this;
    };

    template <typename T, uint8_t Mask>
    inline T value() const;
    template <typename T>
    inline T value() const;

    inline const as_union &as() const {
      return *reinterpret_cast<const as_union *>(
        (reinterpret_cast<const char *>(this)) + 1
      );
    };

    uint8_t type_;
    size_t length_;
  };

  struct stack_part {
    stack_part(Local<Array> o, uint32_t l) : array(o), i(0), len(l) {}
    stack_part(Local<Object> o, uint32_t l) : object(o), i(0), len(l) {}

    inline Local<Value> value() const {
      if (object.IsEmpty()) return array;
      else                  return object;
    }

    inline void child(Local<Value> v) {
      if (object.IsEmpty()) array->Set(i++, v);
      else if (key.IsEmpty()) key = v;
      else {
        object->Set(key, v);
        key.Clear();
        i++;
      }
    }

    Local<Object> object;
    Local<Array> array;
    Local<Value> key;

    uint32_t i;
    uint32_t len;
  };

private:
  inline bool next() {
    if      (size_ <  offset_) throw bad_data();
    else if (size_ == offset_) return false;
    part_ = reinterpret_cast<part *>(const_cast<char *>(head_) + offset_);
    offset_ += sizeof(part_->type());
    return true;
  }

private:
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
  inline size_t unpack(Local<Value> &result) {
    std::stack<stack_part> stack;

    while (next()) {
      Local<Value> v;
      uint32_t l = 0;
      uint8_t t = part_->type();

      switch (t) {
        case 0xc0: // nil
          v = Local<Value>::New(Null());  break;
        case 0xc2: // false
          v = Local<Value>::New(False()); break;
        case 0xc3: // true
          v = Local<Value>::New(True());  break;

        case 0xca: // float 32
          v = Number::New(value<float>());  break;
        case 0xcb: // float 64
          v = Number::New(value<double>()); break;

        case 0x00 ... 0x7f: // positive fixint
          v = Integer::New(fix<0xff>()); break;
        case 0xcc: // uint 8
          v = Integer::NewFromUnsigned(value<uint8_t>()); break;
        case 0xcd: // uint 16
          v = Integer::NewFromUnsigned(value<uint16_t>()); break;
        case 0xce: // uint 32
          v = Integer::NewFromUnsigned(value<uint32_t>()); break;
        case 0xcf: // uint 64
          v = Number::New(value<uint64_t>()); break;

        case 0xe0 ... 0xff: // negative fixint
          v = Integer::New(fix()); break;
        case 0xd0: // int 8
          v = Integer::New(value<int8_t>()); break;
        case 0xd1: // int 16
          v = Integer::New(value<int16_t>()); break;
        case 0xd2: // int 32
          v = Integer::New(value<int32_t>()); break;
        case 0xd3: // int 64
          v = Number::New(value<int64_t>()); break;

        case 0xa0 ... 0xbf: // fixstr
          v = String::New(data<0x1f>(), length_); break;
        case 0xd9: // str 8
          v = String::New(data<uint8_t>(), length_); break;
        case 0xda: // str 16
          v = String::New(data<uint16_t>(), length_); break;
        case 0xdb: // str 32
          v = String::New(data<uint32_t>(), length_); break;

        case 0x90 ... 0x9f: // fixarray
          l = fix<0x0f>(); t = 0xdd; break;
        case 0xdc: // array 16
          l = value<uint16_t>(); t = 0xdd; break;
        case 0xdd: // array 32
          l = value<uint32_t>(); break;

        case 0x80 ... 0x8f: // fixmap
          l = fix<0x0f>(); t = 0xdf; break;
        case 0xde: // map 16
          l = value<uint16_t>(); t = 0xdf; break;
        case 0xdf: // map 32
          l = value<uint32_t>(); break;

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

      if (0 < l) {
        switch (t) {
          case 0xdf: {
            Local<Object> o = Object::New();
            if (!stack.empty()) stack.top().child(o);
            stack.push(stack_part(o, l));
            break;
          }
          case 0xdd: {
            Local<Array> a = Array::New(l);
            if (!stack.empty()) stack.top().child(a);
            stack.push(stack_part(a, l));
            break;
          }
        }
        continue;
      }

      switch (t) {
        case 0xdf: v = Object::New(); break;
        case 0xdd: v = Array::New();  break;
      }

      if (stack.empty()) {
        result = v;
        goto end;
      }

      stack_part &parent = stack.top();
      parent.child(v);

      if (parent.len <= parent.i) {
        stack.pop();
        if (stack.empty()) {
          result = parent.value();
          goto end;
        }
      }
    }

    if (!stack.empty()) {
      result = stack.top().value();
    } else {
      result = Local<Value>::New(Undefined());
    }

end:
    return offset_;
  }

private:
  part  *part_;
  const char *head_;
  const size_t size_;
  size_t length_;
  size_t offset_;
};

template<> inline  uint8_t Unpacker::part::fix<0xff>() const { return type_; }
template<> inline  uint8_t Unpacker::part::value() const { return as().u8; }
template<> inline uint16_t Unpacker::part::value() const { return ntohs(as().u16); }
template<> inline uint32_t Unpacker::part::value() const { return ntohl(as().u32); }
template<> inline uint64_t Unpacker::part::value() const { return be64toh(as().u64); }
template<> inline   int8_t Unpacker::part::value() const { return as().i8; }
template<> inline  int16_t Unpacker::part::value() const { return value<uint16_t>(); }
template<> inline  int32_t Unpacker::part::value() const { return value<uint32_t>(); }
template<> inline  int64_t Unpacker::part::value() const { return value<uint64_t>(); }
template<> inline float Unpacker::part::value() const {
  uint32_t i = value<uint32_t>();
  return reinterpret_cast<float &>(i);
}
template<> inline double Unpacker::part::value() const {
  uint64_t i = value<uint64_t>();
  return reinterpret_cast<double &>(i);
}

} // namespace node_msgpack

#endif

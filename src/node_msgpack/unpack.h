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

    template <typename T>
    inline const T as() const {
      return *reinterpret_cast<const T *>(
        (reinterpret_cast<const char *>(this)) + 1
      );
    };

    uint8_t type_;
    size_t length_;
  };

  struct stack_part {
    stack_part(uint32_t l, bool o = false)
    : i(0), len(o ? l * 2 : l), is_object(o) {
      if (is_object) {
        object = Object::New();
      } else {
        array = Array::New(len);
      }
    }

    inline Local<Value> value() const {
      if (is_object) return object;
      return array;
    }

    inline void child(Local<Value> v) {
      if (!is_object) {
        array->Set(i, v);
      } else if (key.IsEmpty()) {
        key = v;
      } else {
        object->Set(key, v);
        key.Clear();
      }
      ++i;
    }

    Local<Array> array;
    Local<Object> object;
    Local<Value> key;

    uint32_t i;
    const uint32_t len;
    const bool is_object;
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
  inline size_t Unpack(Local<Value> &result) {
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
        stack.push(stack_part(l, t == 0xdf));
        continue;
      }

      switch (t) {
        case 0xdf: v = Object::New(); break;
        case 0xdd: v = Array::New(); break;
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
        } else {
          stack.top().child(parent.value());
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

template<> inline uint8_t Unpacker::part::fix<0xff>() const { return type_; }

#define VALUE(T, code) \
  template<> inline T Unpacker::part::value() const { return (code); }

VALUE(uint8_t, as<uint8_t>());
VALUE(uint16_t, ntohs(as<uint16_t>()));
VALUE(uint32_t, ntohl(as<uint32_t>()));
VALUE(uint64_t, be64toh(as<uint64_t>()));
VALUE(int8_t, as<int8_t>());
VALUE(int16_t, value<uint16_t>());
VALUE(int32_t, value<uint32_t>());
VALUE(int64_t, value<uint64_t>());

#undef VALUE

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

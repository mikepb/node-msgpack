#pragma once

#ifndef _NODE_MSGPACK_H_
#define _NODE_MSGPACK_H_

#include <assert.h>
#include <stdio.h>

#include <tr1/array>
#include <deque>
#include <stack>
#include <vector>

#include <msgpack.hpp>
#include <node.h>
#include <node_buffer.h>
#include "nan.h"

// http://stackoverflow.com/questions/809902/64-bit-ntohl-in-c
// https://gist.github.com/yinyin/2027912
#if defined(__linux__)
#  include <endian.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#  include <sys/endian.h>
#elif defined(__OpenBSD__)
#  include <sys/types.h>
#  define be16toh(x) betoh16(x)
#  define be32toh(x) betoh32(x)
#  define be64toh(x) betoh64(x)
#elif defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#  define htobe16(x) OSSwapHostToBigInt16(x)
#  define htole16(x) OSSwapHostToLittleInt16(x)
#  define be16toh(x) OSSwapBigToHostInt16(x)
#  define le16toh(x) OSSwapLittleToHostInt16(x)
#  define htobe32(x) OSSwapHostToBigInt32(x)
#  define htole32(x) OSSwapHostToLittleInt32(x)
#  define be32toh(x) OSSwapBigToHostInt32(x)
#  define le32toh(x) OSSwapLittleToHostInt32(x)
#  define htobe64(x) OSSwapHostToBigInt64(x)
#  define htole64(x) OSSwapHostToLittleInt64(x)
#  define be64toh(x) OSSwapBigToHostInt64(x)
#  define le64toh(x) OSSwapLittleToHostInt64(x)
#endif

#define NODE_MSGPACK_FN_TOSTRING    0x0001
#define NODE_MSGPACK_RE_TOSTRING    0x0002
#define NODE_MSGPACK_CALL_TOJSON    0x0004
#define NODE_MSGPACK_DATE_DOUBLE    0x0008

using namespace v8;
using namespace node;

namespace node_msgpack {

// keep around 1MB of used buffers, assuming 8096-byte pages
static std::tr1::array<msgpack_sbuffer *, 128> sbuffers;
static uint sbuffers_count = 0;

static msgpack_sbuffer *sbuffer_factory();
static void sbuffer_release(msgpack_sbuffer *sb);
static void sbuffer_callback(char *data, void *hint);

class circular_structure : public std::exception {
public:
  virtual const char *what() const throw() {
    return "circular_structure";
  }
};

class type_error : public std::exception {
public:
  virtual const char *what() const throw() {
    return "type_error";
  }
};

class msgpack_error : public std::exception {
public:
  msgpack_error(const char *msg) : msg_(msg) {}
  virtual const char *what() const throw() {
    return msg_;
  }
private:
  const char *msg_;
};

class bad_data : public msgpack_error {
public:
  bad_data() : msgpack_error("bad_data") {}
};

class MessagePack {

private:
  struct pakdat {
      pakdat(Local<Value> v, msgpack_object *o, int i = 0)
          : val(v), mo(o), id(i) {}
      Local<Value> val;
      msgpack_object *mo;
      int id;
  };

public:
  MessagePack()
  : flags_(NODE_MSGPACK_CALL_TOJSON | NODE_MSGPACK_RE_TOSTRING)
  {
  }

  ~MessagePack() {
  }

public:
  Local<Value> Pack(Local<Value> val);
  Local<Value> Pack(Local<Array> stream);
  static Local<Value> Unpack(Local<Value> val);

#if (NODE_MODULE_VERSION > 0x000B)
  Local<Value> Pack(const FunctionCallbackInfo<Value>& args);
#else
  Local<Value> Pack(const Arguments& args);
#endif

public:
  inline void SetFlags(uint32_t flags, bool onoff = true) {
    if (onoff) flags_ |= flags;
    else       flags_ ^= flags;
  }

private:
  void unpakdat();
  void insert_if_absent(Local<Value> val);
  void erase_id(int id);
  void object_to_json();
  void apply_replacer();
  void write_buffer();

private:
  void init();
  void clear();
  void try_pack();
  void pack();
  void pack_string();
  void pack_result(Local<String> sym);
  void pack_number();
  void pack_boolean();
  void pack_nil();
  void pack_date();
  void pack_function();
  void pack_regexp();
  void pack_buffer();
  void pack_array();
  void pack_object();

private:
  uint32_t flags_;
  msgpack::zone zone_;
  msgpack_object *mo_;
  std::tr1::unordered_set<int> id_hashes_;
  std::vector<pakdat> stack_;
  std::vector<msgpack_object> objects_;
  Local<String> to_string_;
  Local<String> to_iso_string_;
  Local<String> to_json_;
  Local<Value> val_;
};

inline Local<Value> MessagePack::Pack(Local<Value> val) {
  init();
  objects_.resize(1);
  stack_.push_back(pakdat(val, &objects_.front()));
  try_pack();
  return val_;
}

inline Local<Value> MessagePack::Pack(Local<Array> stream) {
  init();
  int len = stream->Length();
  objects_.resize(len);
  stack_.clear();
  stack_.reserve(len);
  while (0 < len--)
    stack_.push_back(pakdat(stream->Get(len), &objects_[len]));
  try_pack();
  return val_;
}

#if (NODE_MODULE_VERSION > 0x000B)
inline Local<Value> MessagePack::Pack(const FunctionCallbackInfo<Value>& args)
#else
inline Local<Value> MessagePack::Pack(const Arguments& args)
#endif
{
  init();
  int len = args.Length();
  objects_.resize(len);
  stack_.clear();
  stack_.reserve(len);
  while (0 < len--)
    stack_.push_back(pakdat(args[len], &objects_[len]));
  try_pack();
  return val_;
}

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
  return be16toh(as().u16);
}

template<>
inline uint32_t msgpack_part::value() const {
  return be32toh(as().u32);
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

inline Local<Value> MessagePack::Unpack(Local<Value> val) {
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

inline void MessagePack::unpakdat() {
  pakdat &dat = stack_.back();
  erase_id(dat.id);
  val_ = dat.val;
  mo_ = dat.mo;
  stack_.pop_back();
}

inline void MessagePack::insert_if_absent(Local<Value> o) {
  int id = o->ToObject()->GetIdentityHash();
  if (0 < id_hashes_.count(id)) {
    throw circular_structure();
  } else {
      id_hashes_.insert(id);
      if (!stack_.empty()) stack_.back().id = id;
  }
}

inline void MessagePack::erase_id(int id) {
  if (id) id_hashes_.erase(id);
}

inline void MessagePack::object_to_json() {
  if ((flags_ & NODE_MSGPACK_CALL_TOJSON) && val_->IsObject()) {
    Local<Object> o = val_->ToObject();
    if (o->Has(to_json_)) {
      Local<Value> fn = o->Get(to_json_);
      if (fn->IsFunction()) {
        val_ = fn.As<Function>()->Call(o, 0, NULL);
      }
    }
  }
}

inline void MessagePack::apply_replacer() {
  // TODO: handle replacer
}

inline void MessagePack::write_buffer() {
  msgpack_packer pk;
  msgpack_sbuffer *sb = sbuffer_factory();
  msgpack_packer_init(&pk, sb, msgpack_sbuffer_write);

  // packit
  for (std::vector<msgpack_object>::iterator it = objects_.begin();
       it != objects_.end(); it++)
  {
    if (msgpack_pack_object(&pk, *it)) {
      sbuffer_release(sb);
      throw std::bad_alloc();
    }
  }

  val_ = NanNewBufferHandle(sb->data, sb->size, sbuffer_callback, sb);
}

inline void MessagePack::init() {
  id_hashes_.clear();
  stack_.clear();
  objects_.clear();
  to_string_ = NanSymbol("toString");
  to_iso_string_ = NanSymbol("toISOString");
  to_json_ = NanSymbol("toJSON");
}

inline void MessagePack::clear() {
  zone_.clear();
  mo_ = NULL;
  to_iso_string_.Clear();
  to_string_.Clear();
  to_json_.Clear();
  val_.Clear();
}

inline void MessagePack::try_pack() {
  try {
    pack();
  } catch (circular_structure &err) {
    clear();
    throw err;
  } catch (std::bad_alloc &err) {
    clear();
    throw err;
  }
}

inline void MessagePack::pack() {
  while (!stack_.empty()) {
    unpakdat();
    object_to_json();
    apply_replacer();
    assert(!val_.IsEmpty());
    if (val_->IsString() || val_->IsStringObject()) {
      pack_string();
    } else if (val_->IsNumber() || val_->IsNumberObject()) {
      pack_number();
    } else if (val_->IsBoolean() || val_->IsBooleanObject()) {
      pack_boolean();
    } else if (val_->IsNull() || val_->IsUndefined() || val_->IsExternal()) {
      pack_nil();
    } else if (val_->IsDate()) {
      pack_date();
    } else if (Buffer::HasInstance(val_)) {
      pack_buffer();
    } else if (val_->IsFunction()) {
      pack_function();
    } else if (val_->IsRegExp()) {
      pack_regexp();
    } else if (val_->IsObject()) {
      insert_if_absent(val_);
      if (val_->IsArray()) {
        pack_array();
      } else {
        pack_object();
      }
    } else {
      pack_nil();
    }
  }
  write_buffer();
}

inline void MessagePack::pack_string() {
  Local<String> str = val_->ToString();
  size_t len = str->Utf8Length();
  char *buf = (char *)zone_.malloc(len);
  mo_->type = MSGPACK_OBJECT_RAW;
  mo_->via.raw.size = len;
  mo_->via.raw.ptr = buf;
  NanFromV8String(val_, Nan::UTF8, NULL, buf, len);
}

inline void MessagePack::pack_result(Local<String> sym) {
  Local<Object> o = val_->ToObject();
  assert(!o.IsEmpty());
  Local<Value> fn = o->Get(sym);
  assert(fn->IsFunction());
  val_ = fn.As<Function>()->Call(o, 0, NULL);
  pack_string();
}

inline void MessagePack::pack_number() {
  // pack uint
  if (val_->IsUint32()) {
    mo_->via.u64 = val_->Uint32Value();
    mo_->type = MSGPACK_OBJECT_POSITIVE_INTEGER;
  }
  // pack int
  else if (val_->IsInt32()) {
    mo_->via.i64 = val_->IntegerValue();
    mo_->type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
  }
  // pack decimal
  else {
    mo_->type = MSGPACK_OBJECT_DOUBLE;
    mo_->via.dec = val_->NumberValue();
  }
}

inline void MessagePack::pack_boolean() {
  mo_->type = MSGPACK_OBJECT_BOOLEAN;
  mo_->via.boolean = val_->BooleanValue();
}

inline void MessagePack::pack_nil() {
  mo_->type = MSGPACK_OBJECT_NIL;
}

inline void MessagePack::pack_date() {
  if (flags_ & NODE_MSGPACK_DATE_DOUBLE) {
    Local<Date> date = Local<Date>::Cast(val_);
    mo_->type = MSGPACK_OBJECT_DOUBLE;
    mo_->via.dec = date->NumberValue();
  } else {
    pack_result(to_iso_string_);
  }
}

inline void MessagePack::pack_function() {
  if (flags_ & NODE_MSGPACK_FN_TOSTRING) {
    pack_result(to_string_);
  } else {
    pack_nil();
  }
}

inline void MessagePack::pack_regexp() {
  if (flags_ & NODE_MSGPACK_RE_TOSTRING) {
    pack_result(to_string_);
  } else {
    pack_nil();
  }
}

inline void MessagePack::pack_array() {
  Local<Array> a = val_->ToObject().As<Array>();
  size_t len = a->Length();

  mo_->type = MSGPACK_OBJECT_ARRAY;
  mo_->via.array.size = len;

  if (0 < len) {
    msgpack_object *mos = (msgpack_object *)
      zone_.malloc(sizeof(msgpack_object) * len);
    mo_->via.array.ptr = mos;
    // push onto stack in reverse to be popped in order
    stack_.reserve(stack_.size() + len);
    for (uint32_t i = len; 0 < i--;) {
      stack_.push_back(pakdat(a->Get(i), mos + i));
    }
  }
}

inline void MessagePack::pack_object() {
  Local<Object> o = val_->ToObject();
  Local<Array> a = o->GetOwnPropertyNames();
  size_t len = a->Length();

  mo_->type = MSGPACK_OBJECT_MAP;
  mo_->via.map.size = len;

  if (0 < len) {
    msgpack_object_kv *kvs = (msgpack_object_kv *)
      zone_.malloc(sizeof(msgpack_object_kv) * len);
    mo_->via.map.ptr = kvs;

    // push onto stack in reverse to be popped in order
    for (uint32_t i = a->Length(); 0 < i--;) {
      Local<Value> k = a->Get(i);
      Local<Value> v = o->Get(k);
      stack_.reserve(stack_.size() + len * 2);
      // skip function and/or regexp
      if ((!(flags_ & NODE_MSGPACK_FN_TOSTRING) && v->IsFunction()) ||
          (!(flags_ & NODE_MSGPACK_RE_TOSTRING) && v->IsRegExp())) {
        mo_->via.array.size--;
      } else {
        msgpack_object_kv *kv = kvs + i;
        stack_.push_back(pakdat(v, &kv->val));
        stack_.push_back(pakdat(k, &kv->key));
      }
    }
  }
}

inline void MessagePack::pack_buffer() {
  mo_->type = MSGPACK_OBJECT_RAW;
  mo_->via.raw.size = Buffer::Length(val_);
  mo_->via.raw.ptr = Buffer::Data(val_);
}

inline msgpack_sbuffer *sbuffer_factory() {
    if (0 < sbuffers_count) {
        msgpack_sbuffer *sb = sbuffers[--sbuffers_count];
        if (!sb) throw std::bad_alloc();
        msgpack_sbuffer_init(sb);
        return sb;
    } else {
        return msgpack_sbuffer_new();
    }
}

inline void sbuffer_release(msgpack_sbuffer *sb) {
    if (sbuffers_count < sbuffers.size()) {
        sbuffers[sbuffers_count++] = sb;
    } else {
        msgpack_sbuffer_free(sb);
    }
}

void sbuffer_callback(char *data, void *hint) {
    assert(hint);
    msgpack_sbuffer *sb = (msgpack_sbuffer *)hint;
    sbuffer_release(sb);
}

} // namespace node_msgpack

#endif

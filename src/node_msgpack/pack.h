#pragma once

#ifndef _NODE_MSGPACK_PACK_H_
#define _NODE_MSGPACK_PACK_H_

#include <utility>
#include <tr1/unordered_set>
#include <vector>

#include <node.h>
#include <node_buffer.h>

#include "nan.h"
#include "endian.h"

using namespace v8;
using namespace node;

namespace node_msgpack {

class Packer {
public:
  Packer(
    const Flags flags = MSGPACK_FLAGS_NONE,
    Local<Function> replacer = Local<Function>()
  ):
    size_(128),
    flags_(static_cast<Flags>(flags | (replacer.IsEmpty() ? 0 : MSGPACK_HAS_REPLACER))),
    replacer_(replacer)
  {
    head_ = buf_ = reinterpret_cast<char *>(malloc(size_));
    if (!buf_) throw std::bad_alloc();
  }

public:
  void Pack(const Local<Value> &val);
  void Pack(const Local<Array> &stream);
#if (NODE_MODULE_VERSION > 0x000B)
  void Pack(const FunctionCallbackInfo<Value>& args);
#else
  void Pack(const Arguments& args);
#endif

public:
  inline char *Data() const { return buf_; }
  inline size_t Length() const { return head_ - buf_; }

private:
  void PackValue(const Local<Value> &val);
  inline void PackObject(const Local<Value> &val);
  inline void PackArray(const Local<Value> &val);

private:
  inline void reserve(const size_t bytes);
  template <typename T>
  inline void write(const T value);
  template <uint8_t V, typename T>
  inline void value(const T val);
  template <typename T>
  inline void value(const T val);

public:
  inline void PackNil();
  inline void PackTrue();
  inline void PackFalse();
  template <typename T>
  inline void PackNumber(const T val);
  inline void PackString(const size_t len);
  inline void PackString(const char *data, const size_t len);
  inline void PackBinary(const size_t len);
  inline void PackBinary(const char *data, const size_t len);
  inline void PackData(const char *data, const size_t len);
  inline void PackArray(const uint32_t size);
  inline void PackMap(const uint32_t size);

private:
  inline int insert_if_absent(const Local<Value> &o);
  inline void erase_id(const int id);
  inline const Local<Value> to_json(const Local<Value> &val);
  inline void pack_string(const Local<Value> &val);
  inline void pack_result(const Local<Value> &val, const Local<String> &sym);
  inline void pack_number(const Local<Value> &val);
  inline void pack_boolean(const Local<Value> &val);
  inline void pack_date(const Local<Value> &val);
  inline void pack_date_double(const Local<Value> &val);
  inline void pack_regexp(const Local<Value> &val);
  inline void pack_buffer(const Local<Value> &val);

private:
  char *buf_;
  char *head_;
  size_t size_;

private:
  const Flags flags_;
  std::tr1::unordered_set<int> id_hashes_;
  Local<Function> replacer_;
  Local<String> to_iso_string_;
  Local<String> to_json_;
  Local<String> to_string_;
};

inline void Packer::Pack(const Local<Value> &val) {
  PackValue(val);
}

inline void Packer::Pack(const Local<Array> &vals) {
  for (uint32_t i = 0; i < vals->Length(); i++) {
    PackValue(vals->Get(i));
  }
}

#if (NODE_MODULE_VERSION > 0x000B)
inline void Packer::Pack(const FunctionCallbackInfo<Value>& args)
#else
inline void Packer::Pack(const Arguments& args)
#endif
{
  for (int i = 0; i < args.Length(); i++) {
    PackValue(args[i]);
  }
}

void Packer::PackValue(const Local<Value> &v) {
  const Local<Value> val = flags_ & MSGPACK_NO_TOJSON ? v : to_json(v);
  if (val->IsString() || val->IsStringObject()) {
    pack_string(val);
  } else if (val->IsNumber() || val->IsNumberObject()) {
    pack_number(val);
  } else if (val->IsBoolean() || val->IsBooleanObject()) {
    pack_boolean(val);
  } else if (val->IsNull() || val->IsUndefined()) {
    PackNil();
  } else if (val->IsDate()) {
    if (flags_ & MSGPACK_DATE_TO_DOUBLE) pack_date_double(val);
    else pack_date(val);
  } else if (Buffer::HasInstance(val)) {
    pack_buffer(val);
  } else if (val->IsRegExp()) {
    if (flags_ & MSGPACK_REGEXP_TO_STRING) {
      if (to_string_.IsEmpty()) to_string_ = NanSymbol("toString");
      pack_result(val, to_string_);
    } else {
      PackMap(0);
    }
  } else if (val->IsFunction()) {
    if (flags_ & MSGPACK_FUNCTION_TO_STRING) {
      pack_result(val, to_string_);
    } else {
      PackNil();
    }
  } else if (val->IsObject()) {
    if (val->IsArray()) {
      PackArray(val);
    } else {
      PackObject(val);
    }
  } else {
    PackMap(0); /* external, etc */
  }
}

inline void Packer::PackObject(const Local<Value> &val) {
  Local<Object> o = static_cast< Local<Value> >(val).As<Object>();
  Local<Array> a = o->GetOwnPropertyNames();
  uint32_t len = a->Length();

  typedef std::pair< Local<Value>, Local<Value> > kvpair_t;

  std::vector<kvpair_t> kvpairs;
  for (uint32_t i = 0; i < len; i++) {
    Local<Value> k = a->Get(i);
    Local<Value> v = o->Get(k);
    if (v->IsFunction()) continue;
    kvpairs.push_back(std::make_pair(k, v));
  }

  PackMap(kvpairs.size());

  if (0 < kvpairs.size()) {
    int id = insert_if_absent(o);
    for (std::vector<kvpair_t>::iterator it = kvpairs.begin();
         it != kvpairs.end(); ++it)
    {
      PackValue(it->first);
      PackValue(it->second);
    }
    erase_id(id);
  }
}

inline void Packer::PackArray(const Local<Value> &val) {
  Local<Array> a = static_cast< Local<Value> >(val).As<Array>();
  uint32_t len = a->Length();

  PackArray(len);

  if (0 < len) {
    int id = insert_if_absent(a);
    for (uint32_t i = 0; i < len; ++i) {
      PackValue(a->Get(i));
    }
    erase_id(id);
  }
}

inline void Packer::reserve(const size_t bytes) {
  const size_t offset = head_ - buf_;
  const size_t len = offset + bytes;
  if (len < size_) return;
  while (size_ < len) size_ *= 2;
  char *buf = reinterpret_cast<char *>(realloc(buf_, size_));
  if (buf == NULL) {
    free(buf_);
    throw std::bad_alloc();
  } else {
    buf_ = buf;
    head_ = buf_ + offset;
  }
}

template <>
inline void Packer::write<uint8_t>(const uint8_t val) {
  *reinterpret_cast<uint8_t *>(head_) = val;
  head_ += sizeof(uint8_t);
}

template <>
inline void Packer::write<uint16_t>(const uint16_t val) {
  uint16_t *iref = reinterpret_cast<uint16_t *>(head_);
  *iref = htons(val);
  head_ += sizeof(uint16_t);
}

template <>
inline void Packer::write<uint32_t>(const uint32_t val) {
  uint32_t *iref = reinterpret_cast<uint32_t *>(head_);
  *iref = htonl(val);
  head_ += sizeof(uint32_t);
}

template <>
inline void Packer::write<uint64_t>(const uint64_t val) {
  uint64_t *iref = reinterpret_cast<uint64_t *>(head_);
  *iref = htobe64(val);
  head_ += sizeof(uint64_t);
}

template <>
inline void Packer::write<int8_t>(const int8_t val) {
  write<uint8_t>(reinterpret_cast<const uint8_t &>(val));
}

template <>
inline void Packer::write<int16_t>(const int16_t val) {
  write<uint16_t>(reinterpret_cast<const uint16_t &>(val));
}

template <>
inline void Packer::write<int32_t>(const int32_t val) {
  write<uint32_t>(reinterpret_cast<const uint32_t &>(val));
}

template <>
inline void Packer::write<int64_t>(const int64_t val) {
  write<uint64_t>(reinterpret_cast<const uint64_t &>(val));
}

template <>
inline void Packer::write<float>(const float val) {
  write<uint32_t>(reinterpret_cast<const uint32_t &>(val));
}

template <>
inline void Packer::write<double>(const double val) {
  write<uint64_t>(reinterpret_cast<const uint64_t &>(val));
}

template <>
inline void Packer::value<0, uint8_t>(const uint8_t val) {
  reserve(sizeof(int8_t));
  write<uint8_t>(val);
}

template <>
inline void Packer::value<0, int8_t>(const int8_t val) {
  reserve(sizeof(int8_t));
  write<int8_t>(val);
}

template <uint8_t V, typename T>
inline void Packer::value(const T val) {
  reserve(sizeof(uint8_t) + sizeof(T));
  write<uint8_t>(V);
  write<T>(val);
}

template <>
inline void Packer::value<uint8_t>(const uint8_t val) {
  reserve(sizeof(uint8_t));
  write<uint8_t>(val);
}

template<>
inline void Packer::value<int8_t>(const int8_t val) {
  reserve(sizeof(int8_t));
  write<int8_t>(val);
}

inline void Packer::PackNil() {
  value<uint8_t>(0xc0);
}

inline void Packer::PackTrue() {
  value<uint8_t>(0xc3);
}

inline void Packer::PackFalse() {
  value<uint8_t>(0xc2);
}

template <>
inline void Packer::PackNumber<uint8_t>(const uint8_t val) {
  if (val < (1 << 7)) {
    /* fixnum */
    value(val);
  } else {
    /* unsigned 8 */
    value<0xcc, uint8_t>(val);
  }
}

template <>
inline void Packer::PackNumber<uint16_t>(const uint16_t val) {
  if (val < (1 << 8)) {
    PackNumber<uint8_t>(val);
  } else {
    /* unsigned 16 */
    value<0xcd, uint16_t>(val);
  }
}

template <>
inline void Packer::PackNumber<uint32_t>(const uint32_t val) {
  if (val < (1 << 16)) {
    PackNumber<uint16_t>(val);
  } else {
    /* unsigned 32 */
    value<0xce, uint32_t>(val);
  }
}

template <>
inline void Packer::PackNumber<uint64_t>(const uint64_t val) {
  if (val < (1ULL << 32)) {
    PackNumber<uint32_t>(val);
  } else {
    /* unsigned 64 */
    value<0xcf, uint64_t>(val);
  }
}

template <>
inline void Packer::PackNumber<int8_t>(const int8_t val) {
  if (-(1 << 5) <= val) {
    /* fixnum */
    value<int8_t>(val);
  } else {
    /* signed 8 */
    value<0xd0, int8_t>(val);
  }
}

template <>
inline void Packer::PackNumber<int16_t>(const int16_t val) {
  if ((1 << 7) <= val) {
    PackNumber<uint16_t>(val);
  } else if (val < -(1 << 7)) {
    /* signed 16 */
    value<0xd1, int16_t>(val);
  } else {
    PackNumber<int8_t>(val);
  }
}

template <>
inline void Packer::PackNumber<int32_t>(const int32_t val) {
  if ((1 << 7) <= val) {
    PackNumber<uint32_t>(val);
  } else if (val < -(1 << 15)) {
    /* signed 32 */
    value<0xd2, int32_t>(val);
  } else {
    PackNumber<int16_t>(val);
  }
}

template <>
inline void Packer::PackNumber<int64_t>(const int64_t val) {
  if ((1 << 7) <= val) {
    PackNumber<uint64_t>(val);
  } else if (val < -(1LL << 31)) {
    /* signed 64 */
    value<0xd3, int64_t>(val);
  } else {
    PackNumber<int32_t>(val);
  }
}

template <>
inline void Packer::PackNumber<float>(const float val) {
  value<0xca, float>(val);
}

template <>
inline void Packer::PackNumber<double>(const double val) {
  value<0xcb, double>(val);
}

inline void Packer::PackString(const size_t len) {
  if (len < 0x20) {
    reserve(sizeof(uint8_t) + len);
    write<uint8_t>(0xa0 + len);
  } else if (len < 0x100) {
    reserve(sizeof(uint8_t) * 2 + len);
    write<uint8_t>(0xd9);
    write<uint8_t>(len);
  } else if (len < 0x10000) {
    reserve(sizeof(uint8_t) + sizeof(uint16_t) + len);
    write<uint8_t>(0xda);
    write<uint16_t>(len);
  } else if (len < 0x100000000) {
    reserve(sizeof(uint8_t) + sizeof(uint32_t) + len);
    write<uint8_t>(0xdb);
    write<uint32_t>(len);
  } else {
    free(buf_);
    throw type_error();
  }
}

inline void Packer::PackString(const char *data, const size_t len) {
  PackString(len);
  memcpy(reinterpret_cast<char *>(head_), data, len);
  head_ += len;
}

inline void Packer::PackBinary(const size_t len) {
  if (len < 0x100) {
    reserve(sizeof(uint8_t) * 2 + len);
    write<uint8_t>(0xc4);
    write<uint8_t>(len);
  } else if (len < 0x10000) {
    reserve(sizeof(uint8_t) + sizeof(uint16_t) + len);
    write<uint8_t>(0xc5);
    write<uint16_t>(len);
  } else if (len < 0x100000000) {
    reserve(sizeof(uint8_t) + sizeof(uint32_t) + len);
    write<uint8_t>(0xc6);
    write<uint32_t>(len);
  } else {
    free(buf_);
    throw type_error();
  }
}

inline void Packer::PackBinary(const char *data, const size_t len) {
  PackBinary(len);
  memcpy(reinterpret_cast<char *>(head_), data, len);
  head_ += len;
}

inline void Packer::PackData(const char *data, const size_t len) {
  reserve(len);
  memcpy(reinterpret_cast<char *>(head_), data, len);
  head_ += len;
}

inline void Packer::PackArray(const uint32_t size) {
  if (size < 0x10) {
    write<uint8_t>(0x90 + size);
  } else if (size < 0x10000) {
    write<uint8_t>(0xdc);
    write<uint16_t>(size);
  } else {
    write<uint8_t>(0xdd);
    write<uint32_t>(size);
  }
}

inline void Packer::PackMap(const uint32_t size) {
  if (size < 0x10) {
    write<uint8_t>(0x80 + size);
  } else if (size < 0x10000) {
    write<uint8_t>(0xde);
    write<uint16_t>(size);
  } else {
    write<uint8_t>(0xdf);
    write<uint32_t>(size);
  }
}





inline int Packer::insert_if_absent(const Local<Value> &o) {
  int id = static_cast< Local<Value> >(o).As<Object>()->GetIdentityHash();
  if (0 < id_hashes_.count(id)) {
    free(buf_);
    throw circular_structure();
  } else {
    id_hashes_.insert(id);
  }
  return id;
}

inline void Packer::erase_id(const int id) {
  if (id) id_hashes_.erase(id);
}

inline const Local<Value> Packer::to_json(const Local<Value> &v) {
  if (v->IsObject()) {
    Local<Object> o = static_cast< Local<Value> >(v).As<Object>();
    if (to_json_.IsEmpty()) to_json_ = NanSymbol("toJSON");
    Local<Value> fn = o->Get(to_json_);
    if (fn->IsFunction()) return fn.As<Function>()->Call(o, 0, NULL);
  }
  return v;
}


inline void Packer::pack_string(const Local<Value> &val) {
  Local<String> str = static_cast< Local<Value> >(val).As<String>();
  size_t len = str->Utf8Length();
  PackString(len);
  NanFromV8String(str, Nan::UTF8, NULL, reinterpret_cast<char *>(head_), len);
  head_ += len;
}

inline void Packer::pack_result(const Local<Value> &val, const Local<String> &sym) {
  Local<Object> o = static_cast< Local<Value> >(val).As<Object>();
  Local<Value> fn = o->Get(sym);
  pack_string(fn.As<Function>()->Call(o, 0, NULL));
}

inline void Packer::pack_number(const Local<Value> &val) {
  // pack uint
  if (val->IsUint32()) {
    PackNumber<uint32_t>(val->Uint32Value());
  }
  // pack int
  else if (val->IsInt32()) {
    PackNumber<int64_t>(val->IntegerValue());
  }
  // pack decimal
  else {
    PackNumber<double>(val->NumberValue());
  }
}

inline void Packer::pack_boolean(const Local<Value> &val) {
  if (val->BooleanValue()) PackTrue(); else PackFalse();
}

inline void Packer::pack_date(const Local<Value> &val) {
  if (to_iso_string_.IsEmpty())
    to_iso_string_ = NanSymbol("toISOString");
  pack_result(val, to_iso_string_);
}

inline void Packer::pack_date_double(const Local<Value> &val) {
  Local<Date> date = static_cast< Local<Value> >(val).As<Date>();
  write<double>(date->NumberValue());
}

inline void Packer::pack_buffer(const Local<Value> &val) {
  size_t len = Buffer::Length(val);
  PackBinary(len);
  memcpy(head_, Buffer::Data(val), len);
  head_ += len;
}

} // namespace node_msgpack

#endif

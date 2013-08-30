#pragma once

#ifndef _NODE_MSGPACK_PACK_H_
#define _NODE_MSGPACK_PACK_H_

#include <assert.h>

#include <tr1/array>
#include <tr1/unordered_set>
#include <vector>

#include <msgpack.hpp>
#include <node.h>
#include <node_buffer.h>

#include "nan.h"

using namespace v8;
using namespace node;

namespace node_msgpack {

// keep around 1MB of used buffers, assuming 8096-byte pages
static std::tr1::array<msgpack_sbuffer *, 128> sbuffers;
static uint sbuffers_count = 0;

static msgpack_sbuffer *sbuffer_factory();
static void sbuffer_release(msgpack_sbuffer *sb);
static void sbuffer_callback(char *data, void *hint);

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

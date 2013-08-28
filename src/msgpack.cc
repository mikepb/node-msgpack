#include <assert.h>
#include <stdio.h>

#include <set>
#include <stack>


#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>
#include <cmath>
#include <iostream>
#include <vector>
#include "nan.h"

using namespace std;
using namespace v8;
using namespace node;

// An exception class that wraps a textual message
struct MsgpackException {
    MsgpackException(const char *str) : msg(str) {}
    const char *msg;
};

// A holder for a msgpack_zone object; ensures destruction on scope exit
class MsgpackZone {
    public:
        msgpack_zone _mz;

        MsgpackZone(size_t sz = 1024) {
            msgpack_zone_init(&this->_mz, sz);
        }

        ~MsgpackZone() {
            msgpack_zone_destroy(&this->_mz);
        }
};

// Convert a MessagePack object to a V8 object.
//
// This method is recursive. It will probably blow out the stack on objects
// with extremely deep nesting.
static Handle<Value>
msgpack_to_v8(msgpack_object *mo) {
    switch (mo->type) {
    case MSGPACK_OBJECT_NIL:
        return Null();

    case MSGPACK_OBJECT_BOOLEAN:
        return (mo->via.boolean) ?
            True() :
            False();

    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        // As per Issue #42, we need to use the base Number
        // class as opposed to the subclass Integer, since
        // only the former takes 64-bit inputs. Using the
        // Integer subclass will truncate 64-bit values.
        return Number::New(static_cast<double>(mo->via.u64));

    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        // See comment for MSGPACK_OBJECT_POSITIVE_INTEGER
        return Number::New(static_cast<double>(mo->via.i64));

    case MSGPACK_OBJECT_DOUBLE:
        return Number::New(mo->via.dec);

    case MSGPACK_OBJECT_ARRAY: {
        Local<Array> a = Array::New(mo->via.array.size);

        for (uint32_t i = 0; i < mo->via.array.size; i++) {
            a->Set(i, msgpack_to_v8(&mo->via.array.ptr[i]));
        }

        return a;
    }

    case MSGPACK_OBJECT_RAW:
        return String::New(mo->via.raw.ptr, mo->via.raw.size);

    case MSGPACK_OBJECT_MAP: {
        Local<Object> o = Object::New();

        for (uint32_t i = 0; i < mo->via.map.size; i++) {
            o->Set(
                msgpack_to_v8(&mo->via.map.ptr[i].key),
                msgpack_to_v8(&mo->via.map.ptr[i].val)
            );
        }

        return o;
    }

    default:
        throw MsgpackException("Encountered unknown MesssagePack object type");
    }
}

// This will be passed to Buffer::New so that we can manage our own memory.
static void
sbuffer_destroy_callback(char *data, void *hint) {
    assert(hint);
    msgpack_sbuffer *sbuffer = (msgpack_sbuffer *)hint;
    msgpack_sbuffer_free(sbuffer);
}

static inline void
pack_v8string(msgpack_packer *pk, Local<Value> val) {
    assert(val->IsString());

    Local<String> str = val->ToString();
    size_t len = str->Utf8Length();

    int err = msgpack_pack_raw(pk, len);
    assert(!err);

    //
    // modified from msgpack sbuffer.h
    //

    msgpack_sbuffer *sb = (msgpack_sbuffer *)pk->data;

    if (sb->alloc - sb->size < len) {
        size_t nsize =
            sb->alloc ? sb->alloc * 2 : MSGPACK_SBUFFER_INIT_SIZE;

        while (nsize < sb->size + len) { nsize *= 2; }

        void *tmp = realloc(sb->data, nsize);
        assert(tmp);

        sb->data = (char *)tmp;
        sb->alloc = nsize;
    }

    size_t wlen = str->WriteUtf8(
        sb->data + sb->size, sb->alloc, NULL, String::NO_NULL_TERMINATION);

    assert(len == wlen);

    sb->size += len;
}

static inline void
pack_v8call(msgpack_packer *pk, Local<Value> val, const char *sym) {
    Local<Object> o = val->ToObject();
    assert(!o.IsEmpty());
    Local<Value> fn = o->Get(NanSymbol("toString"));
    assert(fn->IsFunction());
    Local<Value> str = fn.As<Function>()->Call(o, 0, NULL);
    pack_v8string(pk, str);
}

struct packdat {
    packdat(Local<Value> v) : val(v), popit(false) {}
    packdat(bool p) : popit(p) {}
    Local<Value> val;
    bool popit;
};

// var buf = msgpack.pack(obj[, obj ...]);
//
// Returns a Buffer object representing the serialized state of the provided
// JavaScript object. If more arguments are provided, their serialized state
// will be accumulated to the end of the previous value(s).
//
// Any number of objects can be provided as arguments, and all will be
// serialized to the same bytestream, back-to-back.
NAN_METHOD(Pack) {
    NanScope();

    msgpack_sbuffer *sb = msgpack_sbuffer_new();
    assert(sb);
    msgpack_packer   packer;
    msgpack_packer  *pk = &packer;
    msgpack_packer_init(pk, sb, msgpack_sbuffer_write);

    std::set<int>   id_hash_set;
    std::stack<int> id_hash_stack;
    std::stack<packdat> val_stack;

    assert(1 < args.Length());

    // options
    Local<Object> options;
    if (args[0]->IsObject()) options = args[0]->ToObject();
    // options.dateToNumber
    bool dateToNumber = NanBooleanOptionValue(
        options, NanSymbol("dateToNumber"), false);
    // options.functionToString
    bool functionToString = NanBooleanOptionValue(
        options, NanSymbol("functionToString"), true);
    // options.regexpToString
    bool regexpToString = NanBooleanOptionValue(
        options, NanSymbol("regexpToString"), true);
    // options.callToJSON
    bool callToJSON = NanBooleanOptionValue(
        options, NanSymbol("callToJSON"), true);

    // add arguments to stack
    for (int i = args.Length() - 1; 0 < i; i--) {
        val_stack.push(args[i]);
    }

    // convert to msgpack
    while (!val_stack.empty()) {
        int err = 0;

        // pop val from stack
        packdat dat = val_stack.top();
        val_stack.pop();

        // pop id hash
        if (dat.popit) {
            id_hash_set.erase(id_hash_stack.top());
            id_hash_stack.pop();
            continue;
        }

        // get local value
        Local<Value> val = dat.val;

        // toJSON
        if (callToJSON && val->IsObject()) {
            Local<Object> o = val->ToObject();
            if (o->Has(NanSymbol("toJSON"))) {
                Local<Value> fn = o->Get(NanSymbol("toJSON"));
                if (fn->IsFunction()) {
                    val = fn.As<Function>()->Call(o, 0, NULL);
                }
            }
        }

        // TODO: handle replacer

        // pack string
        if (val->IsString() || val->IsStringObject()) {
            pack_v8string(pk, val);
        }

        // pack int
        else if (val->IsInt32()) {
            err = msgpack_pack_int(pk, val->Int32Value());
            assert(!err);
        }

        // pack int
        else if (val->IsNumber() || val->IsNumberObject()) {
            err = msgpack_pack_double(pk, val->NumberValue());
            assert(!err);
        }

        // pack boolean
        else if (val->IsBoolean() || val->IsBooleanObject()) {
            err = val->BooleanValue()
                ? msgpack_pack_true(pk)
                : msgpack_pack_false(pk);
            assert(!err);
        }

        // pack nil
        else if (val->IsNull() || val->IsUndefined()) {
            err = msgpack_pack_nil(pk);
            assert(!err);
        }

        // pack object
        else if (val->IsObject()) {

            // pack function
            if (val->IsFunction()) {
                if (functionToString) {
                    pack_v8call(pk, val, "toString");
                } else {
                    err = msgpack_pack_nil(pk);
                    assert(!err);
                }
            }

            // pack regexp
            else if (val->IsRegExp()) {
                if (regexpToString) {
                    pack_v8call(pk, val, "toString");
                } else {
                    err = msgpack_pack_nil(pk);
                    assert(!err);
                }
            }

            // pack date
            else if (val->IsDate()) {
                if (dateToNumber) {
                    Local<Date> date = Local<Date>::Cast(val);
                    err = msgpack_pack_double(pk, date->NumberValue());
                    assert(!err);
                } else {
                    pack_v8call(pk, val, "toISOString");
                }
            }

            // pack buffer
            else if (Buffer::HasInstance(val)) {
                err = msgpack_pack_raw(pk, Buffer::Length(val));
                assert(!err);
                err = msgpack_pack_raw_body(
                    pk, Buffer::Data(val), Buffer::Length(val));
                assert(!err);
            }

            // pack object or array
            else {
                Local<Object> o = val->ToObject();

                // recursion check
                int id_hash = o->GetIdentityHash();
                if (0 < id_hash_set.count(id_hash)) {
                    msgpack_sbuffer_free(sb);
                    return NanThrowTypeError("circular_structure");
                } else {
                    id_hash_set.insert(id_hash);
                    id_hash_stack.push(id_hash);
                    val_stack.push(true);
                }

                // pack array
                if (val->IsArray()) {
                    Local<Array> a = val->ToObject().As<Array>();

                    int err = msgpack_pack_array(pk, a->Length());
                    assert(!err);

                    // push onto stack in reverse to be popped in order
                    for (uint32_t i = a->Length(); 0 < i--;) {
                        val_stack.push(a->Get(i));
                    }

                    continue;
                } else {
                    Local<Array> a = o->GetOwnPropertyNames();

                    err = msgpack_pack_map(pk, a->Length());
                    assert(!err);

                    // push onto stack in reverse to be popped in order
                    for (uint32_t i = a->Length(); 0 < i--;) {
                        Local<Value> k = a->Get(i);
                        Local<Value> v = o->Get(k);

                        // skip function and/or regexp
                        if (!functionToString && v->IsFunction()) {
                            // no-op
                        } else if (!regexpToString && v->IsRegExp()) {
                            // no-op
                        } else {
                            val_stack.push(v);
                            val_stack.push(k);
                        }
                    }
                }
            }
        }

        // default to null
        else {
            err = msgpack_pack_nil(pk);
            assert(!err);
        }
    }

    Local<Object> buf = NanNewBufferHandle(
        sb->data, sb->size, sbuffer_destroy_callback, sb);

    NanReturnValue(buf);
}

// var o = msgpack.unpack(buf);
//
// Return the JavaScript object resulting from unpacking the contents of the
// specified buffer. If the buffer does not contain a complete object, the
// undefined value is returned.
Persistent<Object> persistentExports;
NAN_METHOD(Unpack) {
    NanScope();

    if (args.Length() < 0 || !Buffer::HasInstance(args[0])) {
        return NanThrowError("First argument must be a Buffer");
    }

    Local<Object> buf = args[0]->ToObject();

    MsgpackZone mz;
    msgpack_object mo;
    size_t off = 0;

    switch (msgpack_unpack(Buffer::Data(buf), Buffer::Length(buf), &off, &mz._mz, &mo)) {
    case MSGPACK_UNPACK_EXTRA_BYTES: {
        Local<Object> exports =
            NanPersistentToLocal(persistentExports);
        exports->Set(NanSymbol("offset"),
            Integer::New(static_cast<int32_t>(off))
        );
    }
    case MSGPACK_UNPACK_SUCCESS:
        try {
            NanReturnValue(msgpack_to_v8(&mo));
        } catch (MsgpackException e) {
            NanThrowError(e.msg);
        }

    case MSGPACK_UNPACK_CONTINUE:
        NanReturnUndefined();

    default:
        NanThrowError("Error de-serializing object");
    }

    NanReturnUndefined();
}

extern "C" void
init(Handle<Object> exports) {
    // Save `exports` so that we can set `offset`.
    NanAssignPersistent(Object, persistentExports, exports);
    // exports.pack
    exports->Set(NanSymbol("pack"),
        FunctionTemplate::New(Pack)->GetFunction());
    // exports.unpack
    exports->Set(NanSymbol("unpack"),
        FunctionTemplate::New(Unpack)->GetFunction());
}

NODE_MODULE(msgpackBinding, init);
// vim:ts=4 sw=4 et

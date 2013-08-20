#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>
#include <cmath>
#include <iostream>
#include <vector>
#include <stack>
#include <assert.h>
#include "nan.h"

using namespace std;
using namespace v8;
using namespace node;

#define MSGPACK_SBUFFER_POOL_SIZE 50000
static stack<msgpack_sbuffer *> sbuffers;

// MSC does not support C99 trunc function.
#ifdef _MSC_BUILD
double trunc(double d){ return (d>0) ? floor(d) : ceil(d) ; }
#endif

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

#define DBG_PRINT_BUF(buf, name) \
    do { \
        fprintf(stderr, "Buffer %s has %lu bytes:\n", \
            (name), Buffer::Length(buf) \
        ); \
        for (uint32_t i = 0; i * 16 < Buffer::Length(buf); i++) { \
            fprintf(stderr, "  "); \
            for (uint32_t ii = 0; \
                 ii < 16 && (i * 16) + ii < Buffer::Length(buf); \
                 ii++) { \
                fprintf(stderr, "%s%2.2hhx", \
                    (ii > 0 && (ii % 2 == 0)) ? " " : "", \
                    Buffer::Data(buf)[i * 16 + ii] \
                ); \
            } \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

// This will be passed to Buffer::New so that we can manage our own memory.
static void
FreeMsgpackSbuffer(char *data, void *hint) {
    if (data != NULL && hint != NULL) {
        msgpack_sbuffer *sbuffer = (msgpack_sbuffer *)hint;
        if (sbuffers.size() > MSGPACK_SBUFFER_POOL_SIZE ||
            sbuffer->alloc > (MSGPACK_SBUFFER_INIT_SIZE * 5)) {
            msgpack_sbuffer_free(sbuffer);
        } else {
            sbuffer->size = 0;
            sbuffers.push(sbuffer);
        }
    }
}

// Convert a V8 object to a MessagePack object.
//
// This method is recursive. It will probably blow out the stack on objects
// with extremely deep nesting.
//
// If a circular reference is detected, an exception is thrown.
static void
v8_to_msgpack(Handle<Value> v8obj, msgpack_object *mo, msgpack_zone *mz, size_t depth) {
    if (512 < ++depth) {
        throw MsgpackException("Cowardly refusing to pack object with circular reference");
    }

    if (v8obj->IsUndefined() || v8obj->IsNull()) {
        mo->type = MSGPACK_OBJECT_NIL;
    } else if (v8obj->IsBoolean()) {
        mo->type = MSGPACK_OBJECT_BOOLEAN;
        mo->via.boolean = v8obj->BooleanValue();
    } else if (v8obj->IsNumber()) {
        double d = v8obj->NumberValue();
        if (trunc(d) != d) {
            mo->type = MSGPACK_OBJECT_DOUBLE;
            mo->via.dec = d;
        } else if (d > 0) {
            mo->type = MSGPACK_OBJECT_POSITIVE_INTEGER;
            mo->via.u64 = static_cast<uint64_t>(d);
        } else {
            mo->type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
            mo->via.i64 = static_cast<int64_t>(d);
        }
    } else if (v8obj->IsString()) {
        size_t len = v8obj->ToString()->Utf8Length();

        mo->type = MSGPACK_OBJECT_RAW;
        mo->via.raw.size = static_cast<uint32_t>(len);
        mo->via.raw.ptr = (char*) msgpack_zone_malloc(mz, mo->via.raw.size);

        NanFromV8String(v8obj, Nan::UTF8, NULL,
            const_cast<char *>(mo->via.raw.ptr), len);
    } else if (v8obj->IsDate()) {
        // could be handled by that.toJSON()
        Handle<Date> date = Handle<Date>::Cast(v8obj);
        Local<Function> fn = date->Get(NanSymbol("toISOString")).As<Function>();
        Local<Value> str = fn->Call(date, 0, NULL);
        size_t len = str->ToString()->Utf8Length();

        mo->type = MSGPACK_OBJECT_RAW;
        mo->via.raw.size = static_cast<uint32_t>(len);
        mo->via.raw.ptr = (char*) msgpack_zone_malloc(mz, mo->via.raw.size);

        NanFromV8String(str, Nan::UTF8, NULL,
            const_cast<char *>(mo->via.raw.ptr), len);
    } else if (v8obj->IsArray()) {
        Local<Object> o = v8obj->ToObject();
        Local<Array> a = o.As<Array>();

        mo->type = MSGPACK_OBJECT_ARRAY;
        mo->via.array.size = a->Length();
        mo->via.array.ptr = (msgpack_object*) msgpack_zone_malloc(
            mz,
            sizeof(msgpack_object) * mo->via.array.size
        );

        for (uint32_t i = 0; i < a->Length(); i++) {
            Local<Value> v = a->Get(i);
            v8_to_msgpack(v, &mo->via.array.ptr[i], mz, depth);
        }
    } else if (Buffer::HasInstance(v8obj)) {
        mo->type = MSGPACK_OBJECT_RAW;
        mo->via.raw.size = static_cast<uint32_t>(Buffer::Length(v8obj));
        mo->via.raw.ptr = Buffer::Data(v8obj);
    } else {
        Local<Object> o = v8obj->ToObject();
        Local<Value> fn = o->Get(NanSymbol("toJSON"));

        // for o.toJSON()
        if (fn->IsFunction()) {
            v8_to_msgpack(fn.As<Function>()->Call(o, 0, NULL), mo, mz, depth);
            return;
        }

        Local<Array> a = o->GetPropertyNames();

        mo->type = MSGPACK_OBJECT_MAP;
        mo->via.map.size = a->Length();
        mo->via.map.ptr = (msgpack_object_kv*) msgpack_zone_malloc(
            mz,
            sizeof(msgpack_object_kv) * mo->via.map.size
        );

        for (uint32_t i = 0; i < a->Length(); i++) {
            Local<Value> k = a->Get(i);

            v8_to_msgpack(k, &mo->via.map.ptr[i].key, mz, depth);
            v8_to_msgpack(o->Get(k), &mo->via.map.ptr[i].val, mz, depth);
        }
    }
}

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

    msgpack_packer pk;
    MsgpackZone mz;
    msgpack_sbuffer *sb;

    if (!sbuffers.empty()) {
        sb = sbuffers.top();
        sbuffers.pop();
    } else {
        sb = msgpack_sbuffer_new();
    }

    msgpack_packer_init(&pk, sb, msgpack_sbuffer_write);

    for (int i = 0; i < args.Length(); i++) {
        msgpack_object mo;

        try {
            v8_to_msgpack(args[i], &mo, &mz._mz, 0);
        } catch (MsgpackException e) {
            FreeMsgpackSbuffer(sb->data, sb);
            return NanThrowError(e.msg);
        }

        if (msgpack_pack_object(&pk, mo)) {
            FreeMsgpackSbuffer(sb->data, sb);
            return NanThrowError("Error serializing object");
        }
    }

    // Use Object returned from Buffer::New
    // https://github.com/joyent/node/issues/5864
    NanReturnValue(NanNewBufferHandle(
        sb->data, sb->size, FreeMsgpackSbuffer, (void *)sb
    ));
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

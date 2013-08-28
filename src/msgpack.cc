#include <assert.h>

#include <stack>
#include <tr1/unordered_set>

#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>

#include "nan.h"

using namespace v8;
using namespace node;

// global options
static bool v8date_to_double = false;
static bool v8function_to_string = false;
static bool v8regexp_to_string = true;
static bool v8object_call_to_json = true;

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

static inline int
pack_v8string(msgpack_object *mo, msgpack_zone *mz, Local<Value> val) {
    assert(val->IsString());

    Local<String> str = val->ToString();
    size_t len = str->Utf8Length();
    char *buf = (char *)msgpack_zone_malloc(mz, len);

    if (!buf) return -1;

    mo->type = MSGPACK_OBJECT_RAW;
    mo->via.raw.size = static_cast<uint32_t>(len);
    mo->via.raw.ptr = buf;

    NanFromV8String(val, Nan::UTF8, NULL, buf, len);

    return 0;
}

static inline int
pack_v8call(msgpack_object *mo, msgpack_zone *mz, Local<Value> val, Local<String> sym) {
    Local<Object> o = val->ToObject();
    assert(!o.IsEmpty());
    Local<Value> fn = o->Get(sym);
    assert(fn->IsFunction());
    Local<Value> str = fn.As<Function>()->Call(o, 0, NULL);
    return pack_v8string(mo, mz, str);
}

struct packdat {
    packdat(Local<Value> v, msgpack_object *o, int id = 0)
        : val(v), mo(o), erase_id(id) {}

    Local<Value> val;
    msgpack_object *mo;
    int erase_id;
};

NAN_METHOD(SetOptions) {
    NanScope();

    assert(args.Length() == 1);

    Local<String> date_to_num_sym = NanSymbol("dateToNum");
    Local<String> fn_to_string_sym = NanSymbol("fnToString");
    Local<String> re_to_string_sym = NanSymbol("reToString");
    Local<String> to_json_sym = NanSymbol("toJSON");

    // options
    Local<Object> options;
    if (args[0]->IsObject()) options = args[0]->ToObject();
    if (options->Has(date_to_num_sym))
        v8date_to_double = options->Get(date_to_num_sym)->BooleanValue();
    if (options->Has(fn_to_string_sym))
        v8function_to_string = options->Get(fn_to_string_sym)->BooleanValue();
    if (options->Has(re_to_string_sym))
        v8regexp_to_string = options->Get(re_to_string_sym)->BooleanValue();
    if (options->Has(to_json_sym))
        v8object_call_to_json = options->Get(to_json_sym)->BooleanValue();

    NanReturnUndefined();
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

    Local<String> to_json_sym = NanSymbol("toJSON");
    Local<String> to_string_sym = NanSymbol("toString");
    Local<String> to_iso_string_sym = NanSymbol("toISOString");

    // msgpack intermediate structures
    msgpack_zone mz;
    if (!msgpack_zone_init(&mz, MSGPACK_ZONE_CHUNK_SIZE))
        return NanThrowError("alloc_error");
    msgpack_object *root_mos = (msgpack_object *)
        msgpack_zone_malloc(&mz, sizeof(msgpack_object) * args.Length());
    if (!root_mos) return NanThrowError("alloc_error");

    // recursive
    std::stack<packdat> val_stack;
    std::tr1::unordered_set<int> id_hash_set;

    // add arguments to stack
    for (int i = args.Length() - 1; 0 <= i; i--) {
        val_stack.push(packdat(args[i], root_mos + i));
    }

    // convert to msgpack
    while (!val_stack.empty()) {
        int err = 0;

        // pop val from stack
        packdat dat = val_stack.top();
        val_stack.pop();

        // pop id hash
        if (dat.erase_id) id_hash_set.erase(dat.erase_id);

        // get local value
        Local<Value> val = dat.val;
        msgpack_object *mo = dat.mo;

        // toJSON
        if (v8object_call_to_json && val->IsObject()) {
            Local<Object> o = val->ToObject();
            if (o->Has(to_json_sym)) {
                Local<Value> fn = o->Get(to_json_sym);
                if (fn->IsFunction()) {
                    val = fn.As<Function>()->Call(o, 0, NULL);
                }
            }
        }

        // TODO: handle replacer

        // pack string
        if (val->IsString() || val->IsStringObject()) {
            err = pack_v8string(mo, &mz, val);
        }

        // pack uint
        else if (val->IsUint32()) {
            mo->via.u64 = val->Uint32Value();
            mo->type = MSGPACK_OBJECT_POSITIVE_INTEGER;
        }

        // pack int
        else if (val->IsInt32()) {
            mo->via.i64 = val->IntegerValue();
            mo->type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
        }

        // pack decimal
        else if (val->IsNumber() || val->IsNumberObject()) {
            mo->type = MSGPACK_OBJECT_DOUBLE;
            mo->via.dec = val->NumberValue();
        }

        // pack boolean
        else if (val->IsBoolean() || val->IsBooleanObject()) {
            mo->type = MSGPACK_OBJECT_BOOLEAN;
            mo->via.boolean = val->BooleanValue();
        }

        // pack nil
        else if (val->IsNull() || val->IsUndefined()) {
            mo->type = MSGPACK_OBJECT_NIL;
        }

        // pack object
        else if (val->IsObject()) {

            // pack function
            if (val->IsFunction()) {
                if (v8function_to_string) {
                    err = pack_v8call(mo, &mz, val, to_string_sym);
                } else {
                    mo->type = MSGPACK_OBJECT_NIL;
                }
            }

            // pack regexp
            else if (val->IsRegExp()) {
                if (v8regexp_to_string) {
                    err = pack_v8call(mo, &mz, val, to_string_sym);
                } else {
                    mo->type = MSGPACK_OBJECT_NIL;
                }
            }

            // pack date
            else if (val->IsDate()) {
                if (v8date_to_double) {
                    Local<Date> date = Local<Date>::Cast(val);
                    mo->type = MSGPACK_OBJECT_DOUBLE;
                    mo->via.dec = date->NumberValue();
                } else {
                    err = pack_v8call(mo, &mz, val, to_iso_string_sym);
                }
            }

            // pack buffer
            else if (Buffer::HasInstance(val)) {
                mo->type = MSGPACK_OBJECT_RAW;
                mo->via.raw.size = static_cast<uint32_t>(Buffer::Length(val));
                mo->via.raw.ptr = Buffer::Data(val);
            }

            // pack object or array
            else {
                Local<Object> o = val->ToObject();

                // recursion check
                int id_hash = o->GetIdentityHash();
                if (0 < id_hash_set.count(id_hash)) {
                    return NanThrowTypeError("circular_structure");
                } else {
                    id_hash_set.insert(id_hash);
                    if (!val_stack.empty())
                        val_stack.top().erase_id = id_hash;
                }

                // pack array
                if (val->IsArray()) {
                    Local<Array> a = val->ToObject().As<Array>();
                    size_t len = a->Length();
                    msgpack_object *mos = (msgpack_object *)
                        msgpack_zone_malloc(&mz, sizeof(msgpack_object) * len);

                    mo->type = MSGPACK_OBJECT_ARRAY;
                    mo->via.array.size = len;
                    mo->via.array.ptr = mos;

                    // push onto stack in reverse to be popped in order
                    if (!err) for (uint32_t i = a->Length(); 0 < i--;) {
                        val_stack.push(packdat(a->Get(i), mos + i));
                    }
                } else {
                    Local<Array> a = o->GetOwnPropertyNames();
                    size_t len = a->Length();
                    msgpack_object_kv *kvs = (msgpack_object_kv *)
                        msgpack_zone_malloc(&mz, sizeof(msgpack_object_kv) * len);

                    mo->type = MSGPACK_OBJECT_MAP;
                    mo->via.map.size = len;
                    mo->via.map.ptr = kvs;

                    // push onto stack in reverse to be popped in order
                    if (!err) for (uint32_t i = a->Length(); 0 < i--;) {
                        Local<Value> k = a->Get(i);
                        Local<Value> v = o->Get(k);

                        // skip function and/or regexp
                        if ((!v8function_to_string && v->IsFunction()) ||
                            (!v8regexp_to_string && v->IsRegExp())) {
                            mo->via.array.size--;
                        } else {
                            msgpack_object_kv *kv = kvs + i;
                            val_stack.push(packdat(v, &kv->val));
                            val_stack.push(packdat(k, &kv->key));
                        }
                    }
                }
            }
        }

        // default to null
        else {
            mo->type = MSGPACK_OBJECT_NIL;
        }

        if (err) {
            msgpack_zone_destroy(&mz);
            return NanThrowError("alloc_error");
        }
    }

    // packing stuctures
    msgpack_packer pk;
    msgpack_sbuffer *sb = msgpack_sbuffer_new();
    int err = 0;

    if (!sb) {
        msgpack_zone_destroy(&mz);
        return NanThrowError("alloc_error");
    }

    msgpack_packer_init(&pk, sb, msgpack_sbuffer_write);

    // packit
    for (int i = 0; i < args.Length(); i++) {
        err = msgpack_pack_object(&pk, root_mos[i]);
        if (err) {
            msgpack_sbuffer_destroy(sb);
            msgpack_zone_destroy(&mz);
            return NanThrowError("alloc_error");
        }
    }

    msgpack_zone_destroy(&mz);

    Local<Object> buf = NanNewBufferHandle(
        sb->data, sb->size, sbuffer_destroy_callback, sb);

    NanReturnValue(buf);
}

// var o = msgpack.unpack(buf);
//
// Return the JavaScript object resulting from unpacking the contents of the
// specified buffer. If the buffer does not contain a complete object, the
// undefined value is returned.
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
    case MSGPACK_UNPACK_EXTRA_BYTES:
    case MSGPACK_UNPACK_SUCCESS:
        buf->Set(NanSymbol("offset"), Integer::New(off));

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
    // exports.pack
    exports->Set(NanSymbol("pack"),
        FunctionTemplate::New(Pack)->GetFunction());
    // exports.unpack
    exports->Set(NanSymbol("unpack"),
        FunctionTemplate::New(Unpack)->GetFunction());
    // exports.setOptions
    exports->Set(NanSymbol("setOptions"),
        FunctionTemplate::New(SetOptions)->GetFunction());
}

NODE_MODULE(msgpackBinding, init);
// vim:ts=4 sw=4 et

#include <assert.h>

#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>

#include "node_msgpack.h"
#include "nan.h"

using namespace v8;
using namespace node;
using namespace node_msgpack;

static MessagePack global_msgpack;

NAN_METHOD(SetOptions) {
    NanScope();

    assert(args.Length() == 1);
    // assert(args[0]->IsUint32());

    uint32_t onflags = 0;
    uint32_t offlags = 0;

    Local<String> date_to_num_sym = NanSymbol("dateToNum");
    Local<String> fn_to_string_sym = NanSymbol("fnToString");
    Local<String> re_to_string_sym = NanSymbol("reToString");
    Local<String> to_json_sym = NanSymbol("toJSON");

    // options
    Local<Object> options;
    if (args[0]->IsObject()) options = args[0]->ToObject();
    if (options->Has(date_to_num_sym)) {
        if (options->Get(date_to_num_sym)->BooleanValue())
            onflags &= NODE_MSGPACK_DATE_DOUBLE;
        else
            offlags &= NODE_MSGPACK_DATE_DOUBLE;
    }
    if (options->Has(fn_to_string_sym)) {
        if (options->Get(fn_to_string_sym)->BooleanValue())
            onflags &= NODE_MSGPACK_FN_TOSTRING;
        else
            offlags &= NODE_MSGPACK_FN_TOSTRING;
    }
    if (options->Has(re_to_string_sym)) {
        if (options->Get(re_to_string_sym)->BooleanValue())
            onflags &= NODE_MSGPACK_RE_TOSTRING;
        else
            offlags &= NODE_MSGPACK_RE_TOSTRING;
    }
    if (options->Has(to_json_sym)) {
        if (options->Get(to_json_sym)->BooleanValue())
            onflags &= NODE_MSGPACK_CALL_TOJSON;
        else
            offlags &= NODE_MSGPACK_CALL_TOJSON;
    }

    global_msgpack.SetFlags(onflags);
    global_msgpack.SetFlags(offlags, false);

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

    try {
        if (args.Length() == 1) {
            NanReturnValue(global_msgpack.Pack(args[0]));
        } else {
            NanReturnValue(global_msgpack.Pack(args));
        }
    } catch (std::exception &e) {
        return NanThrowError(e.what());
    }
}

// var o = msgpack.unpack(buf);
//
// Return the JavaScript object resulting from unpacking the contents of the
// specified buffer. If the buffer does not contain a complete object, the
// undefined value is returned.
NAN_METHOD(Unpack) {
    NanScope();

    assert(args.Length() == 1);
    assert(Buffer::HasInstance(args[0]));

    Local<Value> result;

    try {
        result = Unpack(args[0]);
    } catch (msgpack_error &e) {
        return NanThrowError(e.what());
    }

    NanReturnValue(result);
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

#include <assert.h>

#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>

#include "node_msgpack.h"
#include "nan.h"

using namespace v8;
using namespace node;
using namespace node_msgpack;

void free_buffer(char *data, void *hint) {
    free(data);
}

// var buf = msgpack.pack(obj, replacer, hint);
//
// Returns a Buffer object representing the serialized state of the provided
// JavaScript object.
NAN_METHOD(Pack) {
    NanScope();
    try {
        Packer packer;
        packer.Pack(args);
        Local<Value> buf = NanNewBufferHandle(
            packer.Data(), packer.Length(), free_buffer, NULL);
        NanReturnValue(buf);
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
    Local<Object> buf = args[0]->ToObject();

    size_t len = Buffer::Length(buf);
    if (len == 0) NanReturnUndefined();

    Unpacker unpacker(Buffer::Data(buf), len);

    try {
        len = unpacker.Unpack(result);
    } catch (msgpack_error &e) {
        return NanThrowError(e.what());
    }

    buf->Set(NanSymbol("offset"), Integer::New(len));
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
}

NODE_MODULE(msgpackBinding, init);
// vim:ts=4 sw=4 et

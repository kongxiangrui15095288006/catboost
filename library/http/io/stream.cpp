#include "stream.h"
#include "chunk.h"

#if defined(ENABLE_GPL)
#include <library/streams/lz/lz.h>
#endif

#include <library/streams/brotli/brotli.h>
#include <library/streams/lzma/lzma.h>
#include <library/streams/bzip2/bzip2.h>

#include <library/blockcodecs/stream.h>
#include <library/blockcodecs/codecs.h>

#include <util/stream/zlib.h>
#include <util/stream/length.h>
#include <util/stream/null.h>
#include <util/stream/multi.h>

#include <util/system/compat.h>
#include <util/system/yassert.h>

#include <util/network/socket.h>

#include <util/string/cast.h>
#include <util/string/strip.h>

#include <util/generic/string.h>
#include <util/generic/utility.h>
#include <util/generic/hash_set.h>
#include <util/generic/yexception.h>

#define HEADERCMP(header, str) \
    case sizeof(str) - 1:      \
        if (!stricmp(~(header).Name(), str))

namespace {
    static inline size_t SuggestBufferSize() {
        return 8192;
    }

    static inline TStringBuf Trim(const char* b, const char* e) noexcept {
        return StripString(TStringBuf(b, e));
    }

    static inline TStringBuf RmSemiColon(const TStringBuf& s) {
        return s.Before(';');
    }

    template <class T, size_t N>
    class TStreams: private TNonCopyable {
        struct TDelete {
            inline void operator()(T* t) noexcept {
                delete t;
            }
        };

        typedef T* TPtr;

    public:
        inline TStreams() noexcept
            : Beg_(T_ + N)
        {
        }

        inline ~TStreams() {
            TDelete f;

            ForEach(f);
        }

        template <class S>
        inline S* Add(S* t) noexcept {
            return (S*)AddImpl((T*)t);
        }

        template <class Functor>
        inline void ForEach(Functor& f) {
            const TPtr* end = T_ + N;

            for (TPtr* cur = Beg_; cur != end; ++cur) {
                f(*cur);
            }
        }

        TPtr Top() {
            const TPtr* end = T_ + N;
            return end == Beg_ ? nullptr : *Beg_;
        }

    private:
        inline T* AddImpl(T* t) noexcept {
            Y_ASSERT(Beg_ > T_);

            return (*--Beg_ = t);
        }

    private:
        TPtr T_[N];
        TPtr* Beg_;
    };

    template <class TStream>
    class TLazy: public IOutputStream {
    public:
        TLazy(IOutputStream* out, ui16 bs)
            : Output_(out)
            , BlockSize_(bs)
        {
        }

        void DoWrite(const void* buf, size_t len) override {
            ConstructSlave();
            Slave_->Write(buf, len);
        }

        void DoFlush() override {
            ConstructSlave();
            Slave_->Flush();
        }

        void DoFinish() override {
            ConstructSlave();
            Slave_->Finish();
        }

    private:
        inline void ConstructSlave() {
            if (!Slave_) {
                Slave_.Reset(new TStream(Output_, BlockSize_));
            }
        }

    private:
        IOutputStream* Output_;
        ui16 BlockSize_;
        THolder<IOutputStream> Slave_;
    };

    struct TCodecFactory {
        using TDecoderConstructor = std::function<IInputStream*(IInputStream*)>;
        using TEncoderConstructor = std::function<IOutputStream*(IOutputStream*)>;

        inline TCodecFactory() {
            auto gzip = [](auto s) {
                return new TZLibDecompress(s);
            };

            Add("gzip", gzip, [](auto s) { return new TZLibCompress(s, ZLib::GZip); });
            Add("deflate", gzip, [](auto s) { return new TZLibCompress(s, ZLib::ZLib); });
            Add("br", [](auto s) { return new TBrotliDecompress(s); }, [](auto s) { return new TBrotliCompress(s, 4); });
            Add("x-gzip", gzip, [](auto s) { return new TZLibCompress(s, ZLib::GZip); });
            Add("x-deflate", gzip, [](auto s) { return new TZLibCompress(s, ZLib::ZLib); });

#if defined(ENABLE_GPL)
            const ui16 bs = 32 * 1024;

            Add("y-lzo", [](auto s) { return new TLzoDecompress(s); }, [bs](auto s) { return new TLazy<TLzoCompress>(s, bs); });
            Add("y-lzf", [](auto s) { return new TLzfDecompress(s); }, [bs](auto s) { return new TLazy<TLzfCompress>(s, bs); });
            Add("y-lzq", [](auto s) { return new TLzqDecompress(s); }, [bs](auto s) { return new TLazy<TLzqCompress>(s, bs); });
#endif

            Add("y-bzip2", [](auto s) { return new TBZipDecompress(s); }, [](auto s) { return new TBZipCompress(s); });
            Add("y-lzma", [](auto s) { return new TLzmaDecompress(s); }, [](auto s) { return new TLzmaCompress(s); });

            for (auto codecName : NBlockCodecs::ListAllCodecs()) {
                if (codecName.StartsWith("zstd06")) {
                    continue;
                }

                if (codecName.StartsWith("zstd08")) {
                    continue;
                }

                auto codec = NBlockCodecs::Codec(codecName);

                auto enc = [codec](auto s) {
                    return new NBlockCodecs::TCodedOutput(s, codec, 32 * 1024);
                };

                auto dec = [codec](auto s) {
                    return new NBlockCodecs::TDecodedInput(s, codec);
                };

                Add(TString("z-") + codecName, dec, enc);
            }
        }

        inline void Add(TStringBuf name, TDecoderConstructor d, TEncoderConstructor e) {
            Strings.emplace_back(name);
            Codecs[Strings.back()] = TCodec{d, e};
            BestCodecs.emplace_back(~Strings.back());
        }

        static inline TCodecFactory& Instance() noexcept {
            return *SingletonWithPriority<TCodecFactory, 0>();
        }

        inline THolder<IInputStream> Construct(TStringBuf name, IInputStream* slave) const {
            if (auto codec = Codecs.FindPtr(name)) {
                return codec->Decoder(slave);
            }

            return nullptr;
        }

        inline const TEncoderConstructor* FindEncoder(TStringBuf name) const {
            if (auto codec = Codecs.FindPtr(name)) {
                return &codec->Encoder;
            }

            return nullptr;
        }

        struct TCodec {
            TDecoderConstructor Decoder;
            TEncoderConstructor Encoder;
        };

        TVector<TString> Strings;
        THashMap<TStringBuf, TCodec> Codecs;
        TVector<const char*> BestCodecs;
    };
}

class THttpInput::TImpl {
    typedef THashSet<TString> TAcceptCodings;

public:
    inline TImpl(IInputStream* slave)
        : Slave_(slave)
        , Buffered_(Slave_, SuggestBufferSize())
        , ChunkedInput_(nullptr)
        , Input_(nullptr)
        , FirstLine_(Buffered_.ReadLine())
        , Headers_(&Buffered_)
        , KeepAlive_(false)
        , HasContentLength_(false)
        , ContentLength_(0)
        , ContentEncoded_(false)
        , Expect100Continue_(false)
    {
        BuildInputChain();
        Y_ASSERT(Input_);
    }

    inline ~TImpl() {
    }

    inline size_t Read(void* buf, size_t len) {
        return Perform(len, [this, buf](size_t toRead) { return Input_->Read(buf, toRead); });
    }

    inline size_t Skip(size_t len) {
        return Perform(len, [this](size_t toSkip) { return Input_->Skip(toSkip); });
    }

    inline const TString& FirstLine() const noexcept {
        return FirstLine_;
    }

    inline const THttpHeaders& Headers() const noexcept {
        return Headers_;
    }

    inline const TMaybe<THttpHeaders>& Trailers() const noexcept {
        return Trailers_;
    }

    inline bool IsKeepAlive() const noexcept {
        return KeepAlive_;
    }

    inline bool AcceptEncoding(const TString& s) const {
        return Codings_.find(to_lower(s)) != Codings_.end();
    }

    inline bool GetContentLength(ui64& value) const noexcept {
        if (HasContentLength_) {
            value = ContentLength_;
            return true;
        }
        return false;
    }

    inline bool ContentEncoded() const noexcept {
        return ContentEncoded_;
    }

    inline bool HasContent() const noexcept {
        return HasContentLength_ || ChunkedInput_;
    }

    inline bool HasExpect100Continue() const noexcept {
        return Expect100Continue_;
    }

private:
    template <class Operation>
    inline size_t Perform(size_t len, const Operation& operation) {
        size_t processed = operation(len);
        if (processed == 0 && len > 0) {
            if (!ChunkedInput_) {
                Trailers_.ConstructInPlace();
            } else {
                // Read the header of the trailing chunk. It remains in
                // the TChunkedInput stream if the HTTP response is compressed.
                char symbol;
                if (ChunkedInput_->Read(&symbol, 1) != 0) {
                    ythrow THttpParseException() << "some data remaining in TChunkedInput";
                }
            }
        }
        return processed;
    }

    struct TParsedHeaders {
        bool Chunked = false;
        bool KeepAlive = false;
        TStringBuf LZipped;
    };

    struct TTrEnc {
        inline void operator()(const TStringBuf& s) {
            if (s == AsStringBuf("chunked")) {
                p->Chunked = true;
            }
        }

        TParsedHeaders* p;
    };

    struct TAccCoding {
        inline void operator()(const TStringBuf& s) {
            c->insert(ToString(s));
        }

        TAcceptCodings* c;
    };

    template <class Functor>
    inline void ForEach(TString in, Functor& f) {
        in.to_lower();

        const char* b = in.begin();
        const char* c = b;
        const char* e = in.end();

        while (c != e) {
            if (*c == ',') {
                f(RmSemiColon(Trim(b, c)));
                b = c + 1;
            }

            ++c;
        }

        if (b != c) {
            f(RmSemiColon(Trim(b, c)));
        }
    }

    inline bool IsRequest() const {
        return strnicmp(~FirstLine(), "get", 3) == 0 ||
               strnicmp(~FirstLine(), "post", 4) == 0 ||
               strnicmp(~FirstLine(), "put", 3) == 0 ||
               strnicmp(~FirstLine(), "head", 4) == 0 ||
               strnicmp(~FirstLine(), "delete", 6) == 0;
    }

    inline void BuildInputChain() {
        TParsedHeaders p;

        size_t pos = FirstLine_.rfind(' ');
        // In HTTP/1.1 Keep-Alive is turned on by default
        if (pos != TString::npos && strcmp(FirstLine_.c_str() + pos + 1, "HTTP/1.1") == 0) {
            p.KeepAlive = true; //request
        } else if (strnicmp(~FirstLine_, "HTTP/1.1", 8) == 0) {
            p.KeepAlive = true; //reply
        }

        for (THttpHeaders::TConstIterator h = Headers_.Begin(); h != Headers_.End(); ++h) {
            const THttpInputHeader& header = *h;
            switch (+header.Name()) {
                HEADERCMP(header, "transfer-encoding") {
                    TTrEnc f = {&p};
                    ForEach(header.Value(), f);
                }
                break;
                HEADERCMP(header, "content-encoding") {
                    p.LZipped = header.Value();
                }
                break;
                HEADERCMP(header, "accept-encoding") {
                    TAccCoding f = {&Codings_};
                    ForEach(header.Value(), f);
                }
                break;
                HEADERCMP(header, "content-length") {
                    HasContentLength_ = true;
                    ContentLength_ = FromString(header.Value());
                }
                break;
                HEADERCMP(header, "connection") {
                    // accept header "Connection: Keep-Alive, TE"
                    if (strnicmp(~header.Value(), "keep-alive", 10) == 0) {
                        p.KeepAlive = true;
                    } else if (stricmp(~header.Value(), "close") == 0) {
                        p.KeepAlive = false;
                    }
                }
                HEADERCMP(header, "expect") {
                    auto findContinue = [&](const TStringBuf& s) {
                        if (strnicmp(~s, "100-continue", 13) == 0) {
                            Expect100Continue_ = true;
                        }
                    };
                    ForEach(header.Value(), findContinue);
                }
                break;
            }
        }

        if (p.Chunked) {
            ChunkedInput_ = Streams_.Add(new TChunkedInput(&Buffered_, &Trailers_));
            Input_ = ChunkedInput_;
        } else {
            // disable buffering
            Buffered_.Reset(&Cnull);
            Input_ = Streams_.Add(new TMultiInput(&Buffered_, Slave_));

            if (IsRequest() || HasContentLength_) {
                /*
                 * TODO - we have other cases
                 */
                Input_ = Streams_.Add(new TLengthLimitedInput(Input_, ContentLength_));
            }
        }

        if (auto flt = TCodecFactory::Instance().Construct(p.LZipped, Input_)) {
            ContentEncoded_ = true;
            Input_ = Streams_.Add(flt.Release());
        }

        KeepAlive_ = p.KeepAlive;
    }

private:
    IInputStream* Slave_;

    /*
     * input helpers
     */
    TBufferedInput Buffered_;
    TStreams<IInputStream, 8> Streams_;
    IInputStream* ChunkedInput_;

    /*
     * final input stream
     */
    IInputStream* Input_;

    TString FirstLine_;
    THttpHeaders Headers_;
    TMaybe<THttpHeaders> Trailers_;
    bool KeepAlive_;

    TAcceptCodings Codings_;

    bool HasContentLength_;
    ui64 ContentLength_;

    bool ContentEncoded_;
    bool Expect100Continue_;
};

THttpInput::THttpInput(IInputStream* slave)
    : Impl_(new TImpl(slave))
{
}

THttpInput::~THttpInput() {
}

size_t THttpInput::DoRead(void* buf, size_t len) {
    return Impl_->Read(buf, len);
}

size_t THttpInput::DoSkip(size_t len) {
    return Impl_->Skip(len);
}

const THttpHeaders& THttpInput::Headers() const noexcept {
    return Impl_->Headers();
}

const TMaybe<THttpHeaders>& THttpInput::Trailers() const noexcept {
    return Impl_->Trailers();
}

const TString& THttpInput::FirstLine() const noexcept {
    return Impl_->FirstLine();
}

bool THttpInput::IsKeepAlive() const noexcept {
    return Impl_->IsKeepAlive();
}

bool THttpInput::AcceptEncoding(const TString& coding) const {
    return Impl_->AcceptEncoding(coding);
}

TString THttpInput::BestCompressionScheme() const {
    const auto& bestCodings = TCodecFactory::Instance().BestCodecs;

    if (AcceptEncoding("*")) {
        return bestCodings[0];
    }

    for (const auto& bestCoding : bestCodings) {
        if (AcceptEncoding(bestCoding)) {
            return bestCoding;
        }
    }

    return "identity";
}

bool THttpInput::GetContentLength(ui64& value) const noexcept {
    return Impl_->GetContentLength(value);
}

bool THttpInput::ContentEncoded() const noexcept {
    return Impl_->ContentEncoded();
}

bool THttpInput::HasContent() const noexcept {
    return Impl_->HasContent();
}

bool THttpInput::HasExpect100Continue() const noexcept {
    return Impl_->HasExpect100Continue();
}

class THttpOutput::TImpl {
    class TSizeCalculator: public IOutputStream {
    public:
        inline TSizeCalculator() noexcept {
        }

        ~TSizeCalculator() override {
        }

        void DoWrite(const void* /*buf*/, size_t len) override {
            Length_ += len;
        }

        inline size_t Length() const noexcept {
            return Length_;
        }

    private:
        size_t Length_ = 0;
    };

    enum TState {
        Begin = 0,
        FirstLineSent = 1,
        HeadersSent = 2
    };

    struct TFlush {
        inline void operator()(IOutputStream* s) {
            s->Flush();
        }
    };

    struct TFinish {
        inline void operator()(IOutputStream* s) {
            s->Finish();
        }
    };

public:
    inline TImpl(IOutputStream* slave, THttpInput* request)
        : Slave_(slave)
        , State_(Begin)
        , Output_(Slave_)
        , Request_(request)
        , Version_(1100)
        , ComprSchemas_(nullptr)
        , ComprSchemasLen_(0)
        , KeepAliveEnabled_(false)
        , Finished_(false)
    {
    }

    inline ~TImpl() {
    }

    inline void SendContinue() {
        Output_->Write("HTTP/1.1 100 Continue\r\n\r\n");
        Output_->Flush();
    }

    inline void Write(const void* buf, size_t len) {
        if (Finished_) {
            ythrow THttpException() << "can not write to finished stream";
        }

        if (State_ == HeadersSent) {
            Output_->Write(buf, len);

            return;
        }

        const char* b = (const char*)buf;
        const char* e = b + len;
        const char* c = b;

        while (c != e) {
            if (*c == '\n') {
                Line_.append(b, c);

                if (!Line_.empty() && Line_.back() == '\r') {
                    Line_.pop_back();
                }

                b = c + 1;

                Process(Line_);

                if (State_ == HeadersSent) {
                    Output_->Write(b, e - b);

                    return;
                }

                Line_.clear();
            }

            ++c;
        }

        if (b != c) {
            Line_.append(b, c);
        }
    }

    inline void Flush() {
        TFlush f;
        Streams_.ForEach(f);
        Slave_->Flush(); // see SEARCH-1030
    }

    inline void Finish() {
        if (Finished_) {
            return;
        }

        TFinish f;
        Streams_.ForEach(f);
        Slave_->Finish(); // see SEARCH-1030

        Finished_ = true;
    }

    inline const THttpHeaders& SentHeaders() const noexcept {
        return Headers_;
    }

    inline void EnableCompression(const char** schemas, size_t len) {
        ComprSchemas_ = schemas;
        ComprSchemasLen_ = len;
    }

    inline void EnableKeepAlive(bool enable) {
        KeepAliveEnabled_ = enable;
    }

    inline bool IsCompressionEnabled() const noexcept {
        return ComprSchemas_ && ComprSchemasLen_;
    }

    inline bool IsKeepAliveEnabled() const noexcept {
        return KeepAliveEnabled_;
    }

    inline bool CanBeKeepAlive() const noexcept {
        return SupportChunkedTransfer() && IsKeepAliveEnabled() && (Request_ ? Request_->IsKeepAlive() : true);
    }

private:
    static inline bool IsResponse(const TString& s) noexcept {
        return strnicmp(~s, "HTTP/", 5) == 0;
    }

    static inline bool IsRequest(const TString& s) noexcept {
        return !IsResponse(s);
    }

    inline bool IsHttpRequest() const noexcept {
        return IsRequest(FirstLine_);
    }

    inline bool HasResponseBody() const noexcept {
        if (IsHttpResponse()) {
            if (Request_ && Request_->FirstLine().StartsWith(AsStringBuf("HEAD")))
                return false;
            if (FirstLine_.size() > 9 && strncmp(~FirstLine_ + 9, "204", 3) == 0)
                return false;
            return true;
        }
        return false;
    }

    inline bool IsHttpResponse() const noexcept {
        return IsResponse(FirstLine_);
    }

    inline bool HasRequestBody() const noexcept {
        return strnicmp(~FirstLine_, "POST", 4) == 0 ||
               strnicmp(~FirstLine_, "PUT", 3) == 0;
    }
    static inline size_t ParseHttpVersion(const TString& s) {
        if (s.empty()) {
            ythrow THttpParseException() << "malformed http stream";
        }

        size_t parsed_version = 0;

        if (IsResponse(s)) {
            const char* b = ~s + 5;

            while (*b && *b != ' ') {
                if (*b != '.') {
                    parsed_version *= 10;
                    parsed_version += (*b - '0');
                }

                ++b;
            }
        } else {
            /*
             * s not empty here
             */
            const char* e = s.end() - 1;
            const char* b = s.begin();
            size_t mult = 1;

            while (e != b && *e != '/') {
                if (*e != '.') {
                    parsed_version += (*e - '0') * mult;
                    mult *= 10;
                }

                --e;
            }
        }

        return parsed_version * 100;
    }

    inline void ParseHttpVersion() {
        size_t parsed_version = ParseHttpVersion(FirstLine_);

        if (Request_) {
            parsed_version = Min(parsed_version, ParseHttpVersion(Request_->FirstLine()));
        }

        Version_ = parsed_version;
    }

    inline void Process(const TString& s) {
        Y_ASSERT(State_ != HeadersSent);

        if (State_ == Begin) {
            FirstLine_ = s;
            ParseHttpVersion();
            State_ = FirstLineSent;
        } else {
            if (s.empty()) {
                BuildOutputStream();
                WriteCached();
                State_ = HeadersSent;
            } else {
                AddHeader(s);
            }
        }
    }

    inline void WriteCachedImpl(IOutputStream* s) const {
        s->Write(~FirstLine_, +FirstLine_);
        s->Write("\r\n", 2);
        Headers_.OutTo(s);
        s->Write("\r\n", 2);
        s->Finish();
    }

    inline void WriteCached() {
        size_t buflen = 0;

        {
            TSizeCalculator out;

            WriteCachedImpl(&out);
            buflen = out.Length();
        }

        {
            TBufferedOutput out(Slave_, buflen);

            WriteCachedImpl(&out);
        }

        if (IsHttpRequest() && !HasRequestBody()) {
            /*
             * if this is http request, then send it now
             */

            Slave_->Flush();
        }
    }

    inline bool SupportChunkedTransfer() const noexcept {
        return Version_ >= 1100;
    }

    inline void BuildOutputStream() {
        if (CanBeKeepAlive()) {
            AddOrReplaceHeader(THttpInputHeader("Connection", "Keep-Alive"));
        } else {
            AddOrReplaceHeader(THttpInputHeader("Connection", "Close"));
        }

        if (IsHttpResponse()) {
            if (Request_ && IsCompressionEnabled() && HasResponseBody()) {
                TString scheme = Request_->BestCompressionScheme();
                if (scheme != "identity") {
                    AddOrReplaceHeader(THttpInputHeader("Content-Encoding", scheme));
                    RemoveHeader("Content-Length");
                }
            }

            RebuildStream();
        } else {
            if (IsCompressionEnabled()) {
                AddOrReplaceHeader(THttpInputHeader("Accept-Encoding", BuildAcceptEncoding()));
            }
            if (HasRequestBody()) {
                RebuildStream();
            }
        }
    }

    inline TString BuildAcceptEncoding() const {
        TString ret;

        for (size_t i = 0; i < ComprSchemasLen_; ++i) {
            if (ret) {
                ret += ", ";
            }

            ret += ComprSchemas_[i];
        }

        return ret;
    }

    inline void RebuildStream() {
        bool keepAlive = false;
        const TCodecFactory::TEncoderConstructor* encoder = nullptr;
        bool chunked = false;
        bool haveContentLength = false;

        for (THttpHeaders::TConstIterator h = Headers_.Begin(); h != Headers_.End(); ++h) {
            const THttpInputHeader& header = *h;
            const TString hl = to_lower(header.Name());

            if (hl == AsStringBuf("connection")) {
                keepAlive = to_lower(header.Value()) == AsStringBuf("keep-alive");
            } else if (hl == AsStringBuf("content-encoding")) {
                encoder = TCodecFactory::Instance().FindEncoder(to_lower(header.Value()));
            } else if (hl == AsStringBuf("transfer-encoding")) {
                chunked = to_lower(header.Value()) == AsStringBuf("chunked");
            } else if (hl == AsStringBuf("content-length")) {
                haveContentLength = true;
            }
        }

        if (!haveContentLength && !chunked && (IsHttpRequest() || HasResponseBody()) && SupportChunkedTransfer() && (keepAlive || encoder || IsHttpRequest())) {
            AddHeader(THttpInputHeader("Transfer-Encoding", "chunked"));
            chunked = true;
        }

        if (chunked) {
            Output_ = Streams_.Add(new TChunkedOutput(Output_));
        }

        if (encoder) {
            Output_ = Streams_.Add((*encoder)(Output_));
        }
    }

    inline void AddHeader(const THttpInputHeader& hdr) {
        Headers_.AddHeader(hdr);
    }

    inline void AddOrReplaceHeader(const THttpInputHeader& hdr) {
        Headers_.AddOrReplaceHeader(hdr);
    }

    inline void RemoveHeader(const TString& hdr) {
        Headers_.RemoveHeader(hdr);
    }

private:
    IOutputStream* Slave_;
    TState State_;
    IOutputStream* Output_;
    TStreams<IOutputStream, 8> Streams_;
    TString Line_;
    TString FirstLine_;
    THttpHeaders Headers_;
    THttpInput* Request_;
    size_t Version_;

    const char** ComprSchemas_;
    size_t ComprSchemasLen_;

    bool KeepAliveEnabled_;

    bool Finished_;
};

THttpOutput::THttpOutput(IOutputStream* slave)
    : Impl_(new TImpl(slave, nullptr))
{
}

THttpOutput::THttpOutput(IOutputStream* slave, THttpInput* request)
    : Impl_(new TImpl(slave, request))
{
}

THttpOutput::~THttpOutput() {
    try {
        Finish();
    } catch (...) {
    }
}

void THttpOutput::DoWrite(const void* buf, size_t len) {
    Impl_->Write(buf, len);
}

void THttpOutput::DoFlush() {
    Impl_->Flush();
}

void THttpOutput::DoFinish() {
    Impl_->Finish();
}

const THttpHeaders& THttpOutput::SentHeaders() const noexcept {
    return Impl_->SentHeaders();
}

void THttpOutput::EnableCompression(bool enable) {
    const auto& bestCodings = TCodecFactory::Instance().BestCodecs;

    if (enable) {
        EnableCompression((const char**)~bestCodings, +bestCodings);
    } else {
        EnableCompression(nullptr, 0);
    }
}

void THttpOutput::EnableCompression(const char** schemas, size_t count) {
    Impl_->EnableCompression(schemas, count);
}

void THttpOutput::EnableKeepAlive(bool enable) {
    Impl_->EnableKeepAlive(enable);
}

bool THttpOutput::IsKeepAliveEnabled() const noexcept {
    return Impl_->IsKeepAliveEnabled();
}

bool THttpOutput::IsCompressionEnabled() const noexcept {
    return Impl_->IsCompressionEnabled();
}

bool THttpOutput::CanBeKeepAlive() const noexcept {
    return Impl_->CanBeKeepAlive();
}

void THttpOutput::SendContinue() {
    Impl_->SendContinue();
}

unsigned ParseHttpRetCode(const TStringBuf& ret) {
    const TStringBuf code = StripString(StripString(ret.After(' ')).Before(' '));

    return FromString<unsigned>(~code, +code);
}

void SendMinimalHttpRequest(TSocket& s, const TStringBuf& host, const TStringBuf& request, const TStringBuf& agent, const TStringBuf& from) {
    TSocketOutput so(s);
    THttpOutput output(&so);

    output.EnableKeepAlive(false);
    output.EnableCompression(false);

    const IOutputStream::TPart parts[] = {
        IOutputStream::TPart(AsStringBuf("GET ")),
        IOutputStream::TPart(request),
        IOutputStream::TPart(AsStringBuf(" HTTP/1.1")),
        IOutputStream::TPart::CrLf(),
        IOutputStream::TPart(AsStringBuf("Host: ")),
        IOutputStream::TPart(host),
        IOutputStream::TPart::CrLf(),
        IOutputStream::TPart(AsStringBuf("User-Agent: ")),
        IOutputStream::TPart(agent),
        IOutputStream::TPart::CrLf(),
        IOutputStream::TPart(AsStringBuf("From: ")),
        IOutputStream::TPart(from),
        IOutputStream::TPart::CrLf(),
        IOutputStream::TPart::CrLf(),
    };

    output.Write(parts, sizeof(parts) / sizeof(*parts));
    output.Finish();
}

TArrayRef<const char*> SupportedCodings() {
    return TCodecFactory::Instance().BestCodecs;
}

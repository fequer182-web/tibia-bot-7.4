#pragma once
#include <mutex>
#include <streambuf>

namespace crome {
// No exposed put area: every write and flush reaches the same filebuf lock,
// including cout/cerr and game-thread/worker diagnostics.
class LockedLogBuffer final : public std::streambuf {
public:
    explicit LockedLogBuffer(std::streambuf* sink):sink_(sink) {}
protected:
    std::streamsize xsputn(const char* text,std::streamsize count) override {
        std::lock_guard lock(mutex_);return sink_->sputn(text,count);
    }
    int_type overflow(int_type ch) override {
        std::lock_guard lock(mutex_);
        if(traits_type::eq_int_type(ch,traits_type::eof()))return traits_type::not_eof(ch);
        return sink_->sputc(traits_type::to_char_type(ch));
    }
    int sync() override {std::lock_guard lock(mutex_);return sink_->pubsync();}
private:
    std::streambuf* sink_;
    std::mutex mutex_;
};
}

#pragma once

#include <dhft/Sink.h>

#include <iosfwd>

namespace dhft::io {

// Writes each output event as one line of text. Deterministic: no timestamps, no addresses.
class TextSink : public Sink {
public:
    explicit TextSink(std::ostream& out) noexcept : out_{out} {}
    void on_event(const OutEvent& e) override;

private:
    std::ostream& out_;
};

} // namespace dhft::io

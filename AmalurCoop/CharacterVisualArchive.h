#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// koa.exe 16a400f6...: PartRendering::save F11950, Actor::save D1B300.
// Only the rendering tree is accepted. Transient animation-controller archives
// are omitted so a visual refresh cannot copy an in-flight combat/event queue.
namespace CharacterVisualArchive
{
    constexpr std::size_t MaxBytes = 65536;
    constexpr std::uint32_t MaterialTag = 0x5354414D; // MATS, three texture keys + ten ARGB colors
    constexpr std::size_t MaterialBytes = 56;
    constexpr std::uint32_t ActorReferenceTag = 0x00F86910;
    constexpr std::uint32_t ActorTag = 0x008DE50F;
    constexpr std::uint32_t MotionTag = 0x017D60C9;

    struct Cursor
    {
        const std::vector<std::uint8_t>& Bytes;
        std::size_t At = 0;
        bool Read32(std::uint32_t& value)
        {
            if (At > Bytes.size() || Bytes.size() - At < 4) return false;
            std::memcpy(&value, Bytes.data() + At, 4); At += 4; return true;
        }
        bool Chunk(std::uint32_t tag, unsigned version, std::size_t& end)
        {
            std::uint32_t actual = 0, length = 0;
            if (!Read32(actual) || actual != tag || At >= Bytes.size() || Bytes[At++] != version ||
                !Read32(length) || length > Bytes.size() - At) return false;
            end = At + length; return true;
        }
    };
    inline void Write32(std::vector<std::uint8_t>& out, std::uint32_t value)
    {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), b, b + 4);
    }
    inline std::size_t Begin(std::vector<std::uint8_t>& out, std::uint32_t tag, unsigned version)
    {
        Write32(out, tag); out.push_back(static_cast<std::uint8_t>(version));
        const auto offset = out.size(); Write32(out, 0); return offset;
    }
    inline void End(std::vector<std::uint8_t>& out, std::size_t offset)
    {
        const auto length = static_cast<std::uint32_t>(out.size() - offset - 4);
        std::memcpy(out.data() + offset, &length, 4);
    }
    inline bool Copy(Cursor& in, std::vector<std::uint8_t>& out, std::size_t count)
    {
        if (in.At > in.Bytes.size() || count > in.Bytes.size() - in.At) return false;
        out.insert(out.end(), in.Bytes.begin() + in.At, in.Bytes.begin() + in.At + count);
        in.At += count; return true;
    }
    inline bool Actor(Cursor& in, std::vector<std::uint8_t>& out, unsigned depth,
        unsigned& nodes, bool stripMotion, bool appearanceKey = false)
    {
        if (depth > 16 || ++nodes > 64) return false;
        std::size_t refEnd = 0;
        if (!in.Chunk(ActorReferenceTag, 0, refEnd) || in.At >= refEnd) return false;
        const auto ref = Begin(out, ActorReferenceTag, 0);
        const auto present = in.Bytes[in.At++];
        if (present > 1) return false;
        out.push_back(present);
        if (present)
        {
            std::uint32_t modelKey = 0;
            if (!in.Read32(modelKey) || !modelKey) return false;
            Write32(out, modelKey);
            std::size_t actorEnd = 0;
            if (!in.Chunk(ActorTag, 3, actorEnd) || actorEnd > refEnd) return false;
            const auto actor = Begin(out, ActorTag, 3);
            // D2DAC0 identifies these as current/previous world transforms.
            // Canonicalize them to AC38E0's identity (scale=1, flags=0), so
            // walking does not resend/rebuild the entire clothing tree.
            if (in.At > in.Bytes.size() || in.Bytes.size() - in.At < 90) return false;
            const auto matrices = out.size();
            out.resize(matrices + 90, 0);
            const float one = 1.0f;
            std::memcpy(out.data() + matrices + 12, &one, 4);
            std::memcpy(out.data() + matrices + 57, &one, 4);
            if (!stripMotion && std::memcmp(in.Bytes.data() + in.At, out.data() + matrices, 90))
                return false;
            in.At += 90;
            // Actor+1D0 includes sender-local visibility and update flags.
            // CCC300 zeros render opacity for mask E2004; D464xx maintains
            // frame visibility/dirty bits. Never import the sender's culling
            // result or rebuild the clothing tree when those bits change.
            // Preserve explicit hidden (bit 4) on attached models (e.g. a
            // sheathed weapon), but the remote player root must stay visible.
            if (!Copy(in, out, 3 * 4)) return false;
            std::uint32_t owner = 0;
            if (!in.Read32(owner)) return false;
            Write32(out, appearanceKey ? 0 : owner);
            std::uint32_t flags = 0;
            if (!in.Read32(flags)) return false;
            constexpr std::uint32_t viewAndUpdateFlags = 0x000EA143u;
            flags &= ~(viewAndUpdateFlags | (depth == 0 ? 4u : 0u));
            Write32(out, appearanceKey ? (flags & 4u) : flags);
            // Stable animation catalog key, not the current playback queue.
            std::uint32_t catalog = 0;
            if (!in.Read32(catalog)) return false;
            Write32(out, appearanceKey ? 0 : catalog);
            if (stripMotion)
            {
                std::size_t motionEnd = 0;
                if (!in.Chunk(MotionTag, 1, motionEnd) || motionEnd > actorEnd) return false;
                in.At = motionEnd;
            }
            else
            {
                // Received visual archives must never carry motion queues.
                if (in.At + 4 <= in.Bytes.size())
                {
                    std::uint32_t next = 0;
                    std::memcpy(&next, in.Bytes.data() + in.At, 4);
                    if (next == MotionTag) return false;
                }
            }
            std::uint32_t children = 0;
            if (!in.Read32(children) || children > 32) return false;
            Write32(out, children);
            for (std::uint32_t i = 0; i < children; ++i)
                if (!Actor(in, out, depth + 1, nodes, stripMotion, appearanceKey)) return false;
            if (in.At != actorEnd) return false;
            End(out, actor);
        }
        if (in.At != refEnd) return false;
        End(out, ref); return true;
    }
    // PartRendering's outer chunk tag comes from EF7210(component id 7).
    // Verify it against the supplied executable's tag rather than guessing.
    inline bool Normalize(const std::vector<std::uint8_t>& bytes, std::uint32_t renderingTag,
        bool stripMotion, std::vector<std::uint8_t>& output, bool appearanceKey = false)
    {
        output.clear();
        if (bytes.size() > MaxBytes || bytes.size() < 14) return false;
        Cursor in{ bytes }; std::size_t end = 0;
        if (!in.Chunk(renderingTag, 0, end) || (end != bytes.size() && end + MaterialBytes != bytes.size())) return false;
        const auto chunk = Begin(output, renderingTag, 0);
        if (in.At >= end) return false;
        // F11950 serializes C7+F1; bit 0 instructs F1DA50/F757C0 to
        // hide the entire render tree. This is sender-local player visibility.
        output.push_back(static_cast<std::uint8_t>(in.Bytes[in.At++] & ~1u));
        std::uint32_t overrides = 0;
        if (!in.Read32(overrides) || overrides > 128) return false;
        Write32(output, overrides);
        if (!Copy(in, output, overrides * 8)) return false;
        unsigned nodes = 0;
        if (!Actor(in, output, 0, nodes, stripMotion, appearanceKey) || in.At != end) return false;
        End(output, chunk);
        if (end != bytes.size())
        {
            std::uint32_t tag = 0;
            if (!in.Read32(tag) || tag != MaterialTag) return false;
            Write32(output, tag);
            if (!Copy(in, output, MaterialBytes - 4)) return false;
        }
        return true;
    }
}

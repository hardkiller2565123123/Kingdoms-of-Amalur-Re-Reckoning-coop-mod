#pragma once
#include "NetworkProtocol.h"
#include "CharacterVisualArchive.h"
#include <algorithm>
#include <vector>

namespace AmalurNet
{
    struct VisualTransfer
    {
        std::uint32_t Revision = 0;
        std::uint32_t TotalBytes = 0;
        std::vector<std::uint8_t> Bytes;
        std::vector<bool> Received;
        bool Accept(const Packet& packet)
        {
            if (!packet.VisualRevision || !packet.VisualBytes ||
                packet.VisualBytes > CharacterVisualArchive::MaxBytes) return false;
            const auto count = (packet.VisualBytes + sizeof(packet.VisualData) - 1) / sizeof(packet.VisualData);
            const auto offset = packet.VisualIndex * sizeof(packet.VisualData);
            if (packet.VisualCount != count || packet.VisualIndex >= count) return false;
            if (Revision != packet.VisualRevision)
            {
                if (Revision && !IsNewer(packet.VisualRevision, Revision)) return false;
                Revision = packet.VisualRevision; TotalBytes = packet.VisualBytes;
                Bytes.assign(TotalBytes, 0); Received.assign(count, false);
            }
            if (packet.VisualBytes != TotalBytes) return false;
            const auto size = std::min(sizeof(packet.VisualData), Bytes.size() - offset);
            // A duplicate must agree byte for byte with the first copy.
            if (Received[packet.VisualIndex] &&
                std::memcmp(Bytes.data() + offset, packet.VisualData, size)) return false;
            std::memcpy(Bytes.data() + offset, packet.VisualData, size);
            Received[packet.VisualIndex] = true;
            return true;
        }
        bool Complete() const
        {
            return !Received.empty() && std::all_of(Received.begin(), Received.end(), [](bool b) { return b; });
        }
    };
}

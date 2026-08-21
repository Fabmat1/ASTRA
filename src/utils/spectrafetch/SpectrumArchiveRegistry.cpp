// src/utils/spectrafetch/SpectrumArchiveRegistry.cpp

#include "SpectrumArchiveRegistry.h"

#include "ApogeeArchiveClient.h"
#include "EsoArchiveClient.h"
#include "LamostArchiveClient.h"
#include "MastArchiveClient.h"
#include "SdssOpticalArchiveClient.h"

SpectrumArchiveRegistry& SpectrumArchiveRegistry::instance() {
    static SpectrumArchiveRegistry reg;
    return reg;
}

SpectrumArchiveRegistry::SpectrumArchiveRegistry() {
    _clients.push_back(std::make_unique<EsoArchiveClient>());
    _clients.push_back(std::make_unique<LamostArchiveClient>(false));   // LRS
    _clients.push_back(std::make_unique<LamostArchiveClient>(true));    // MRS
    _clients.push_back(std::make_unique<SdssOpticalArchiveClient>());
    _clients.push_back(std::make_unique<MastArchiveClient>());
    _clients.push_back(std::make_unique<ApogeeArchiveClient>());
}

SpectrumArchiveClient* SpectrumArchiveRegistry::clientFor(
    SpecFetch::Archive a) const {
    for (const auto& c : _clients)
        if (c->archive() == a) return c.get();
    return nullptr;
}

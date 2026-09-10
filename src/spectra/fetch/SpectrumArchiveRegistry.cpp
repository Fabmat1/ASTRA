// src/utils/spectrafetch/SpectrumArchiveRegistry.cpp

#include "spectra/fetch/SpectrumArchiveRegistry.h"

#include "spectra/fetch/ApogeeArchiveClient.h"
#include "spectra/fetch/EsoArchiveClient.h"
#include "spectra/fetch/LamostArchiveClient.h"
#include "spectra/fetch/MastArchiveClient.h"
#include "spectra/fetch/SdssOpticalArchiveClient.h"

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

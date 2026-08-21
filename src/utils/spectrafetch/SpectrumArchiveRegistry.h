// src/utils/spectrafetch/SpectrumArchiveRegistry.h
//
// Owns the archive client singletons. Clients are stateless apart from their
// configuration constants, so one instance each serves every session.

#ifndef SPECTRUMARCHIVEREGISTRY_H
#define SPECTRUMARCHIVEREGISTRY_H

#include "SpectrumArchiveTypes.h"

#include <memory>
#include <vector>

class SpectrumArchiveClient;

class SpectrumArchiveRegistry {
public:
    static SpectrumArchiveRegistry& instance();

    SpectrumArchiveClient* clientFor(SpecFetch::Archive a) const;
    const std::vector<std::unique_ptr<SpectrumArchiveClient>>& allClients() const {
        return _clients;
    }

private:
    SpectrumArchiveRegistry();
    std::vector<std::unique_ptr<SpectrumArchiveClient>> _clients;
};

#endif   // SPECTRUMARCHIVEREGISTRY_H

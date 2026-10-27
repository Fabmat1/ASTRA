#include "app/MaintenanceMode.h"

#include "app/StoreMigration.h"
#include "massfit/MassFitPlan.h"

#include <QCoreApplication>
#include <QFile>

#include <cstdio>
#include <cstring>

int validatePlan(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: --validate-plan <plan.json>\n");
        return 2;
    }
    QCoreApplication app(argc, argv);
    QFile f(QString::fromLocal8Bit(argv[2]));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    const auto plan = astra::massfit::MassFitPlan::fromJsonString(QString::fromUtf8(f.readAll()));
    std::printf("%s: %d mode(s), %d setup(s), %d node(s)\n\n%s\n",
                qPrintable(plan.name), int(plan.modes.size()), int(plan.setups.size()),
                int(plan.nodes.size()), qPrintable(astra::massfit::describeTree(plan)));
    const QStringList problems = astra::massfit::validate(plan);
    for (const QString& p : problems) std::printf("problem: %s\n", qPrintable(p));
    std::printf("%s\n", problems.isEmpty() ? "valid" : "INVALID");
    return problems.isEmpty() ? 0 : 1;
}

int runMaintenanceMode(int argc, char** argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--validate-plan") == 0)
        return validatePlan(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--migrate-store") == 0)
        return runStoreMigration(argc, argv);
    return -1;
}

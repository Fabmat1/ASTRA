#include <QtTest/QTest>
#include "../src/models/Project.h"
#include "../src/models/Star.h"

class TestProject : public QObject
{
    Q_OBJECT

private slots:
    void testProjectCreation()
    {
        Project project("Test Project", "A test project");

        QCOMPARE(project.getName(), QString("Test Project"));
        QCOMPARE(project.getDescription(), QString("A test project"));
        QVERIFY(!project.getId().isEmpty());
    }

    void testStarManagement()
    {
        Project project("Test Project");

        auto star1 = std::make_shared<Star>();
        star1->setSourceId("star1");
        star1->setAlias("Star One");

        auto star2 = std::make_shared<Star>();
        star2->setSourceId("star2");
        star2->setAlias("Star Two");

        project.addStar(star1);
        project.addStar(star2);

        QCOMPARE(project.getStarCount(), size_t(2));

        auto retrievedStar = project.getStar("star1");
        QVERIFY(retrievedStar != nullptr);
        QCOMPARE(retrievedStar->getAlias(), QString("Star One"));

        project.removeStar("star2");
        QCOMPARE(project.getStarCount(), size_t(1));
    }
};

QTEST_MAIN(TestProject)
#include "test_project.moc"
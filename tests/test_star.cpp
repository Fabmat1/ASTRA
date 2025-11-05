#include <QtTest/QTest>
#include "../src/models/Star.h"

class TestStar : public QObject
{
    Q_OBJECT

private slots:
    void testStarCreation()
    {
        Star star;
        star.setAlias("Test Star");
        star.setSourceId("123456789");

        QCOMPARE(star.getAlias(), QString("Test Star"));
        QCOMPARE(star.getSourceId(), QString("123456789"));
    }

    void testStarAstrometry()
    {
        Star star;
        star.setRa(100.5);
        star.setDec(-45.2);

        QCOMPARE(star.getRa(), 100.5);
        QCOMPARE(star.getDec(), -45.2);
    }
};

QTEST_MAIN(TestStar)
#include "test_star.moc"
#include <gtest/gtest.h>

#include "earth_engine/layers/CreditSystem.h"

using namespace earth_engine;

TEST(CreditSystemTest, CreateAndRetrieve) {
    CreditSystem cs;
    Credit c = cs.createCredit("Test attribution", false);
    EXPECT_EQ("Test attribution", cs.getHtml(c));
    EXPECT_FALSE(cs.shouldBeShownOnScreen(c));
}

TEST(CreditSystemTest, SetShowOnScreen) {
    CreditSystem cs;
    Credit c = cs.createCredit("Test");
    EXPECT_FALSE(cs.shouldBeShownOnScreen(c));
    cs.setShowOnScreen(c, true);
    EXPECT_TRUE(cs.shouldBeShownOnScreen(c));
}

TEST(CreditSystemTest, DedupReturnsSameCredit) {
    CreditSystem cs;
    Credit a = cs.createCredit("Same text", true);
    Credit b = cs.createCredit("Same text", true);
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.generation, b.generation);
}

TEST(CreditSystemTest, DifferentShowOnScreenIsDifferentCredit) {
    CreditSystem cs;
    Credit a = cs.createCredit("Text", false);
    Credit b = cs.createCredit("Text", true);
    EXPECT_NE(a.id, b.id);
}

TEST(CreditSystemTest, AddRemoveReference) {
    CreditSystem cs;
    Credit c = cs.createCredit("Ref test", true);

    auto snap1 = cs.getSnapshot();
    EXPECT_TRUE(snap1.currentCredits.empty());

    EXPECT_TRUE(cs.addCreditReference(c));

    auto snap2 = cs.getSnapshot();
    ASSERT_EQ(1u, snap2.currentCredits.size());
    EXPECT_EQ(c.id, snap2.currentCredits[0].id);

    EXPECT_TRUE(cs.removeCreditReference(c));

    auto snap3 = cs.getSnapshot();
    EXPECT_TRUE(snap3.currentCredits.empty());
    ASSERT_EQ(1u, snap3.removedCredits.size());
    EXPECT_EQ(c.id, snap3.removedCredits[0].id);
}

TEST(CreditSystemTest, InvalidCreditReturnsFalse) {
    CreditSystem cs;
    Credit invalid{999, 0};
    EXPECT_FALSE(cs.addCreditReference(invalid));
    EXPECT_FALSE(cs.removeCreditReference(invalid));
}

TEST(CreditSystemTest, MultipleCreditsInSnapshot) {
    CreditSystem cs;
    Credit a = cs.createCredit("A", true);
    Credit b = cs.createCredit("B", false);

    cs.addCreditReference(a);
    cs.addCreditReference(b);

    auto snap = cs.getSnapshot();
    ASSERT_EQ(2u, snap.currentCredits.size());
}

TEST(CreditSystemTest, UniqueHtmlFiltering) {
    CreditSystem cs;
    Credit a = cs.createCredit("Same", true);
    Credit b = cs.createCredit("Same", false);

    cs.addCreditReference(a);
    cs.addCreditReference(b);

    // UniqueHtml: only 1 credit for "Same"
    auto snap = cs.getSnapshot(CreditFilteringMode::UniqueHtml);
    ASSERT_EQ(1u, snap.currentCredits.size());
    EXPECT_EQ("Same", cs.getHtml(snap.currentCredits[0]));

    // None: both credits
    auto snap2 = cs.getSnapshot(CreditFilteringMode::None);
    ASSERT_EQ(2u, snap2.currentCredits.size());
}

TEST(CreditSystemTest, RemovedCreditsTracked) {
    CreditSystem cs;
    Credit c = cs.createCredit("Temp", true);
    cs.addCreditReference(c);

    cs.getSnapshot(); // first snapshot — c is active

    cs.removeCreditReference(c);

    auto snap = cs.getSnapshot(); // second snapshot — c is removed
    ASSERT_EQ(1u, snap.removedCredits.size());
    EXPECT_EQ(c.id, snap.removedCredits[0].id);
}

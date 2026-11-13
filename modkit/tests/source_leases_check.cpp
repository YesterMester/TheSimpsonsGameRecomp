#include "source_leases.h"

#include <iostream>

using namespace simpsons::modkit;
namespace {
unsigned checks = 0;
void Require(bool ok, const char* text) {
  ++checks;
  if (!ok) throw std::runtime_error(text);
}
const HostOwner a{1, "creator.first"}, b{1, "creator.second"}, c{1, "creator.third"};
const std::string first = "brt/brt/challenge_mode/challenge_mode_design.str";
const std::string second = "nvr/nvr/zone01/zone01_design.str";
}

int main() {
  try {
    SourceLeases leases;
    leases.Share(a, first);
    leases.Begin(second); leases.Join(b); leases.Join(b); leases.Join(c);
    Require(leases.Claim(a) == first && leases.Claim(b) == second && leases.Size() == 3,
            "An addition preserves another generation's existing source lease");
    leases.Release(c);
    leases.Complete("The new source conflicts");
    Require(leases.Claim(a) == first && leases.Claim(b).empty() && leases.Claim(c).empty(),
            "A failed addition removes only its new claims");
    Require(leases.FailureFor(b)->path == second && !leases.FailureFor(a) && !leases.FailureFor(c),
            "Failures belong to waiting owners, not unrelated or cancelled generations");
    leases.Begin(second); leases.Join(a); leases.Complete("The file is absent");
    Require(leases.Claim(a) == first && leases.FailureFor(a)->path == second,
            "A failed request restores the requester's preceding retained source");
    leases.Retain(a, first);
    Require(!leases.FailureFor(a), "A successful retained spawn clears its owner's old failure");
    leases.Begin(second); leases.Join(b); leases.Retain(b, first); leases.Complete("Invalid addition");
    Require(leases.Claim(b) == first && leases.FailureFor(b),
            "A newly waiting owner that spawned from an old source keeps that resource lease after failure");
    leases.Release(b);
    leases.Begin(second); leases.Join(b); leases.Release(b); leases.Retain(b, first);
    Require(!leases.Wanted(), "Releasing a pending request cancels its request identity");
    leases.Complete("Old request failed");
    Require(leases.Claim(b) == first && !leases.FailureFor(b),
            "An old failure cannot cancel a fresh retained-object lease");
    leases.Begin(second); leases.Join(a); leases.Share(a, first);
    Require(!leases.Wanted(), "Choosing an already loaded source withdraws the pending addition claim");
    leases.Complete("Cancelled request");
    Require(leases.Claim(a) == first && !leases.FailureFor(a),
            "Withdrawal cannot resurrect an error or overwrite the replacement selection");
    leases.Begin(second); leases.Join(a); leases.Release(a); leases.Complete("Unloaded request");
    Require(leases.Claim(a).empty() && !leases.FailureFor(a),
            "A released generation cannot be restored by rollback");
    leases.Begin(second); leases.Join(a); leases.Complete();
    Require(leases.Claim(a) == second && leases.Claim(b) == first && !leases.Wanted(),
            "Committed sources can have independent current selections in the shared session");
    Require(leases.Release(a) && !leases.Empty() && !leases.Release(a),
            "Releasing one owner neither retires another lease nor repeats a release");
    const HostOwner replacement{2, a.mod};
    leases.Share(replacement, second); leases.Release(a);
    Require(leases.Claim(replacement) == second,
            "An old code generation cannot release its replacement's session");
    bool rejected = false;
    leases.Begin(first);
    try { leases.Begin(second); } catch (const Failure& error) { rejected = error.code == "busy"; }
    Require(rejected, "Native source additions are serialized");
    leases.Complete(); leases.Clear();
    leases.Share(a, first); leases.Begin(second); leases.Join(a); leases.Join(b);
    Require(leases.Withdraw(a) && leases.Claim(a) == first && leases.Wanted(),
            "Canceling an addition restores the old lease while another waiter keeps loading");
    leases.Retain(b, first);
    Require(leases.Withdraw(b) && leases.Claim(b) == first && !leases.Wanted(),
            "Cancel preserves an old source acquired by a successful spawn");
    leases.Complete();
    Require(!leases.Withdraw(a) && leases.Claim(a) == first,
            "Canceling without a pending request cannot release a committed source");
    leases.Begin(second); leases.Join(c);
    Require(leases.Withdraw(c) && leases.Claim(c).empty(),
            "A new owner's canceled addition leaves no unneeded session lease");
    leases.Complete(); leases.Clear();
    Require(leases.Empty() && leases.Size() == 0 && !leases.Wanted() && !leases.FailureFor(a),
            "World reset clears all host-only request and lease identities");
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "tutorial_vm_test_internal.hpp"

int main(int argc, char** argv) try {
    if (argc == 2) {
        ss2::vm_test::runRetailChecks(std::filesystem::path(argv[1]));
        ss2::vm_test::runRetailCaofCorpusCheck(std::filesystem::path(argv[1]));
    } else {
        if (const int rc = ss2::vm_test::runCoreAnd4Ato4E(); rc != 0) return rc;
        ss2::vm_test::runPhase4Fto4O();
        ss2::vm_test::runPhase4Pto4W();
        ss2::vm_test::runPhase4Xto5B();
        ss2::vm_test::runPhase5Cto5F();
        ss2::vm_test::runPhase5Gto5J();
        ss2::vm_test::runPhase5Kto5O();
        ss2::vm_test::runPhase5Pto5U();
        ss2::vm_test::runCaofCompleteOpcodes();
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}

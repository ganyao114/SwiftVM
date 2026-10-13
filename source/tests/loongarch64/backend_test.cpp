#include "test_support.h"

int main(int argc, char** argv) {
    using namespace swift::tests::loongarch64;
    try {
        const std::string suite = argc == 1 ? "all" : argv[1];
        if (suite == "all" || suite == "--scalar") {
            ScalarDifferential(); ScalarBits(); HelpersABI();
        }
        if (suite == "all" || suite == "--flags") { FlagsDifferential(); FlagsPublications(); }
        if (suite == "all" || suite == "--control") { ControlFlow(); FunctionCFG(); RuntimeLifecycle(); }
        if (suite == "all" || suite == "--memory") { GuestMemory(); GuestAtomics(); RuntimeFaults(); WideMemory(); }
        if (suite == "all" || suite == "--guest") Arm64Guest();
        if (suite == "all" || suite == "--vector") VectorDifferential();
        if (suite == "all" || suite == "--float") FloatHelpers();
        if (suite == "all" || suite == "--allocation") {
            RegisterAllocationPlan(); ScalarAllocationPlan(); ScalarAllocationExecution(); FlagsPlans();
            SparseHomesPlan(); SparseHomesExecution();
        }
        if (suite == "--sparse-homes") { SparseHomesPlan(); SparseHomesExecution(); }
        if (suite == "--flags-plan") { FlagsPlans(); FlagsPublications(); }
        if (suite == "--flags-publication") FlagsPublications();
        if (suite == "--lifecycle") RuntimeLifecycle();
        if (suite == "all" || suite == "--step") RuntimeSteps();
        if (suite == "all" || suite == "--single-instruction") {
            SingleInstructionAdmissions(); SingleInstructionEncodings(); PrivateSingleInstructionCodes();
        }
        if (suite == "all" || suite == "--compile-contracts") CompileContracts();
        if (suite == "--benchmark") Benchmark();
        if (suite != "all" && suite != "--scalar" && suite != "--flags" &&
            suite != "--control" && suite != "--memory" && suite != "--guest" && suite != "--vector" && suite != "--float" &&
            suite != "--allocation" && suite != "--sparse-homes" && suite != "--flags-plan" &&
            suite != "--flags-publication" && suite != "--lifecycle" && suite != "--step" && suite != "--single-instruction" &&
            suite != "--benchmark" && suite != "--compile-contracts")
            throw std::runtime_error("unknown test suite: " + suite);
        std::cout << "OK " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}

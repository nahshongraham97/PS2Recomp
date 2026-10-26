#include "MiniTest.h"

void register_ps2_runtime_kernel_tests();
void reset_ps2_test_function_table();

int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_runtime_kernel_tests();
    return MiniTest::Run();
}

#include "MiniTest.h"

void register_ps2_sif_register_tests();
void reset_ps2_test_function_table();

int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_sif_register_tests();
    return MiniTest::Run();
}

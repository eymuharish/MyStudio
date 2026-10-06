extern "C" __declspec(dllimport) int MyStudioSecurityDependency();
extern "C" __declspec(dllexport) int MyStudioSecurityProbe() { return MyStudioSecurityDependency(); }

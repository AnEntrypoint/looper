#include "kernel.h"

int main(void)
{
	CKernel Kernel;
	if (!Kernel.Initialize())
	{
		halt();
		return EXIT_HALT;
	}

	TShutdownMode ShutdownMode = Kernel.Run();

	switch (ShutdownMode)
	{
	case ShutdownReboot:
		reboot();
		break;
	default:
		halt();
		break;
	}

	return EXIT_HALT;
}

// Probe: envelope after note-off, exactly as host_test sequences it.
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

int main ()
{
	jm::Engine e;
	e.prepare (48000.0);
	e.setParam (jm::kRevMix, 0.f);
	e.setParam (jm::kDelayMix, 0.f);
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kArmPos, 0.f);

	e.noteOn (64, 0.9f);
	std::vector<float> bl (512), br (512);
	auto run = [&] (int n) {
		float pk = 0.f;
		int done = 0;
		while (done < n)
		{
			const int m = std::min (512, n - done);
			e.process (bl.data (), br.data (), m);
			for (int i = 0; i < m; ++i) pk = std::max (pk, std::fabs (bl[i]));
			done += m;
		}
		return pk;
	};

	std::printf ("note on  0.0s peak %.4f\n", run (512));
	std::printf ("note on  0.5s peak %.4f\n", run (24000));
	e.noteOff (64);
	std::printf ("off      0.0s peak %.4f\n", run (512));
	for (int k = 1; k <= 10; ++k)
		std::printf ("off     %4.0fms peak %.5f\n", k * 50.0, run (2400));
	return 0;
}

/* For symbol_thingey */
#include <chrono>

#define FUTURE_TRACE 0
#include <cps/future.h>
#include <iostream>

using namespace cps;

int
main(void)
{
	auto start = std::chrono::high_resolution_clock::now();
	const int count = 100000;
	auto f2 = future<std::string>::create_shared();
	for(int i = 0; i < count; ++i) {
		auto f = future<std::string>::create_shared();
		auto expected = "happy";
		f->on_done([expected](const std::string &) {
		})->done(expected);
	}
	f2->done("");
	auto elapsed = std::chrono::high_resolution_clock::now() - start;
	std::cout
		<< "Average iteration: "
		<< (std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / (float)count)
		<< " ns"
		<< std::endl;
	std::cout << f2->describe() << std::endl;
	return 0;
}
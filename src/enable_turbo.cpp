#include <iostream>

#include "sysbench/sysbench.hpp"

int main(int argc, char **argv) {
  std::cout << "enable turbo: " << turbo::get_string(turbo::enable()) << "\n";
}
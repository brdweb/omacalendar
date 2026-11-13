// Runs a fuzz target over files and directories of inputs, so the seed
// corpora are a regression test in every build, not only fuzzing builds.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

int runFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 1;
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                         bytes.size());
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  int failures = 0;
  int count = 0;
  for (int index = 1; index < argc; ++index) {
    const std::filesystem::path path(argv[index]);
    if (std::filesystem::is_directory(path)) {
      for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file()) {
          failures += runFile(entry.path());
          ++count;
        }
      }
    } else {
      failures += runFile(path);
      ++count;
    }
  }
  std::printf("replayed %d inputs\n", count);
  return failures == 0 && count > 0 ? 0 : 1;
}

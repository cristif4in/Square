#include <square.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

static void log_fn(void*, int level, const char* msg) {
  fprintf(stderr, "[sq%d] %s\n", level, msg);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: headless <url> [out.png] [vw] [vh] [wait_ms]\n");
    return 2;
  }
  const char* url = argv[1];
  const char* out = argc > 2 ? argv[2] : "page.png";
  int vw = argc > 3 ? atoi(argv[3]) : 1364;
  int vh = argc > 4 ? atoi(argv[4]) : 640;
  int wait_ms = argc > 5 ? atoi(argv[5]) : 15000;

  square_instance* inst = square_create();
  square_set_log_callback(inst, log_fn, nullptr);
  square_set_viewport(inst, vw, vh);
  square_load_url(inst, url);
  auto t0 = std::chrono::steady_clock::now();
  int last_state = -1;
  while (true) {
    int st = square_get_state(inst);
    if (st != last_state) {
      fprintf(stderr, "[headless] state=%d after %lldms\n", st,
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count());
      last_state = st;
    }
    if (st == SQ_STATE_READY || st == SQ_STATE_ERROR) break;
    auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (el > wait_ms) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  int ok = square_save_png(inst, out, 0);
  fprintf(stderr, "[headless] saved=%d %s title='%s' url='%s'\n", ok, out,
          square_get_title(inst) ? square_get_title(inst) : "", square_get_url(inst));
  square_destroy(inst);
  return ok ? 0 : 1;
}

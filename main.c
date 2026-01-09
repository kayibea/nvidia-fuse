#define _XOPEN_SOURCE 500
#define FUSE_USE_VERSION 31
#include <errno.h>
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <nvml.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE 16
#define POLL_INTERVAL 5

struct gpu_stats {
  char vram[BUF_SIZE];
  char temp[BUF_SIZE];
  char util[BUF_SIZE];
};

struct gpu_file {
  const char *name;
  char *buf;
};

static struct gpu_stats gpu;
static struct gpu_file files[] = {
    {"vram", gpu.vram},
    {"temp", gpu.temp},
    {"util", gpu.util},
};

static nvmlDevice_t dev;
static struct fuse *fuse_instance;
static volatile sig_atomic_t running = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *collector_thread(void *arg) {
  (void)arg;

  struct timespec ts = {
      .tv_sec = POLL_INTERVAL,
      .tv_nsec = 0,
  };

  while (running) {
    nvmlReturn_t r;
    nvmlMemory_t mem;
    nvmlUtilization_t util;
    unsigned int temp;

    if ((r = nvmlDeviceGetMemoryInfo(dev, &mem)) != NVML_SUCCESS ||
        (r = nvmlDeviceGetUtilizationRates(dev, &util)) != NVML_SUCCESS ||
        (r = nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp)) !=
            NVML_SUCCESS) {

      fprintf(stderr, "NVML polling failed: %s\n", nvmlErrorString(r));
      nanosleep(&ts, NULL);
      continue;
    }

    if (mem.total == 0) {
      fprintf(stderr, "NVML returned zero total memory\n");
      nanosleep(&ts, NULL);
      continue;
    }

    int vram_perc = (int)((mem.used * 100) / mem.total);

    pthread_mutex_lock(&lock);
    snprintf(gpu.vram, BUF_SIZE, "%d\n", vram_perc);
    snprintf(gpu.util, BUF_SIZE, "%u\n", util.gpu);
    snprintf(gpu.temp, BUF_SIZE, "%u\n", temp);
    pthread_mutex_unlock(&lock);

    nanosleep(&ts, NULL);
  }

  return NULL;
}

static const struct gpu_file *find_file(const char *path) {
  if (path[0] != '/' || path[1] == '\0')
    return NULL;

  const char *name = path + 1;

  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if (!strcmp(name, files[i].name))
      return &files[i];
  }

  return NULL;
}

static int my_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi) {
  (void)fi;
  memset(st, 0, sizeof(*st));

  if (!strcmp(path, "/")) {
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    return 0;
  }

  const struct gpu_file *f = find_file(path);
  if (!f)
    return -ENOENT;

  pthread_mutex_lock(&lock);
  st->st_size = strlen(f->buf);
  pthread_mutex_unlock(&lock);

  st->st_mode = S_IFREG | 0444;
  st->st_nlink = 1;
  return 0;
}

static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
  (void)off;
  (void)fi;
  (void)flags;

  if (strcmp(path, "/"))
    return -ENOENT;

  filler(buf, ".", NULL, 0, 0);
  filler(buf, "..", NULL, 0, 0);

  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    filler(buf, files[i].name, NULL, 0, 0);
  }

  return 0;
}

static int my_open(const char *path, struct fuse_file_info *fi) {
  (void)fi;
  return find_file(path) ? 0 : -ENOENT;
}

static int my_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
  (void)fi;

  const struct gpu_file *f = find_file(path);
  if (!f)
    return -ENOENT;

  char tmp[BUF_SIZE];

  pthread_mutex_lock(&lock);
  snprintf(tmp, sizeof(tmp), "%s", f->buf);
  pthread_mutex_unlock(&lock);

  size_t len = strlen(tmp);
  if ((size_t)off >= len)
    return 0;

  if (off + size > len)
    size = len - off;

  memcpy(buf, tmp + off, size);
  return (int)size;
}

static struct fuse_operations ops = {
    .getattr = my_getattr,
    .readdir = my_readdir,
    .open = my_open,
    .read = my_read,
};

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
  if (fuse_instance)
    fuse_exit(fuse_instance);
}

int main(int argc, char **argv) {
  struct sigaction sa = {0};
  sa.sa_handler = handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  if (nvmlInit() != NVML_SUCCESS) {
    fprintf(stderr, "nvmlInit failed\n");
    return 1;
  }

  if (nvmlDeviceGetHandleByIndex(0, &dev) != NVML_SUCCESS) {
    fprintf(stderr, "nvmlDeviceGetHandleByIndex failed\n");
    nvmlShutdown();
    return 1;
  }

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_cmdline_opts opts;

  if (fuse_parse_cmdline(&args, &opts) != 0) {
    fuse_opt_free_args(&args);
    nvmlShutdown();
    return 1;
  }

  fuse_instance = fuse_new(&args, &ops, sizeof(ops), NULL);
  if (!fuse_instance) {
    fuse_opt_free_args(&args);
    free(opts.mountpoint);
    nvmlShutdown();
    return 1;
  }

  if (fuse_mount(fuse_instance, opts.mountpoint) != 0) {
    fuse_destroy(fuse_instance);
    fuse_opt_free_args(&args);
    free(opts.mountpoint);
    nvmlShutdown();
    return 1;
  }

  pthread_t tid;
  pthread_create(&tid, NULL, collector_thread, NULL);

  fuse_loop(fuse_instance);

  running = 0;
  pthread_join(tid, NULL);

  fuse_unmount(fuse_instance);
  fuse_destroy(fuse_instance);
  fuse_opt_free_args(&args);
  free(opts.mountpoint);

  nvmlShutdown();
  return 0;
}

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
#include <unistd.h>

#define PATH_VRAM "/vram"
#define PATH_UTIL "/util"
#define PATH_TEMP "/temp"

static struct {
  char vram[8];
  char temp[8];
  char util[8];
} gpu;

static nvmlDevice_t dev;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct fuse *fuse_instance = NULL;

static void *collector_thread(void *arg) {
  (void)arg;

  nvmlMemory_t mem;
  nvmlUtilization_t util;
  unsigned int temp;
  while (1) {
    nvmlDeviceGetMemoryInfo(dev, &mem);
    nvmlDeviceGetUtilizationRates(dev, &util);
    nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp);

    int vram_perc = mem.total ? (mem.used * 100) / mem.total : 0;

    pthread_mutex_lock(&lock);
    snprintf(gpu.vram, sizeof(gpu.vram), "%d", vram_perc);
    snprintf(gpu.util, sizeof(gpu.util), "%d", util.gpu);
    snprintf(gpu.temp, sizeof(gpu.temp), "%u", temp);
    pthread_mutex_unlock(&lock);

    usleep(5000 * 1000);
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

  } else if (!strcmp(path, PATH_VRAM) || !strcmp(path, PATH_UTIL) ||
             !strcmp(path, PATH_TEMP)) {

    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = 8;

  } else {
    return -ENOENT;
  }

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
  filler(buf, "vram", NULL, 0, 0);
  filler(buf, "temp", NULL, 0, 0);
  filler(buf, "util", NULL, 0, 0);

  return 0;
}

static int my_open(const char *path, struct fuse_file_info *fi) {
  (void)fi;

  if (!strcmp(path, PATH_VRAM) || !strcmp(path, PATH_UTIL) ||
      !strcmp(path, PATH_TEMP))
    return 0;

  return -ENOENT;
}

static int my_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
  (void)fi;

  const char *src = NULL;

  pthread_mutex_lock(&lock);
  if (!strcmp(path, PATH_VRAM))
    src = gpu.vram;
  else if (!strcmp(path, PATH_TEMP))
    src = gpu.temp;
  else if (!strcmp(path, PATH_UTIL))
    src = gpu.util;
  pthread_mutex_unlock(&lock);

  if (!src)
    return -ENOENT;

  size_t len = strlen(src);

  if ((size_t)off >= len)
    return 0;

  if (off + size > len)
    size = len - off;

  memcpy(buf, src + off, size);
  return size;
}

static struct fuse_operations ops = {
    .getattr = my_getattr,
    .readdir = my_readdir,
    .open = my_open,
    .read = my_read,
};

static void handle_signal(int sig) {
  (void)sig;
  if (fuse_instance)
    fuse_exit(fuse_instance);
}

int main(int argc, char **argv) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  sa.sa_handler = handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  if (nvmlInit() != NVML_SUCCESS) {
    fprintf(stderr, "NVML init failed\n");
    return 1;
  }

  if (nvmlDeviceGetHandleByIndex(0, &dev) != NVML_SUCCESS) {
    fprintf(stderr, "Failed to get device\n");
    return 1;
  }

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_cmdline_opts opts;

  if (fuse_parse_cmdline(&args, &opts) != 0)
    return 1;

  fuse_instance = fuse_new(&args, &ops, sizeof(ops), NULL);
  if (!fuse_instance) {
    fprintf(stderr, "fuse_new failed\n");
    return 1;
  }

  if (fuse_mount(fuse_instance, opts.mountpoint) != 0) {
    fprintf(stderr, "fuse_mount failed\n");
    return 1;
  }

  pthread_t tid;
  pthread_create(&tid, NULL, collector_thread, NULL);

  fuse_loop(fuse_instance);
  fuse_unmount(fuse_instance);
  fuse_destroy(fuse_instance);
  fuse_opt_free_args(&args);
  free(opts.mountpoint);

  if (nvmlShutdown() != NVML_SUCCESS) {
    fprintf(stderr, "NVML Shutdown failed\n");
    return 1;
  }

  return 0;
}

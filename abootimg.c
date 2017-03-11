/* abootimg -  Manipulate (read, modify, create) Android Boot Images
 * Copyright (c) 2010-2011 Gilles Grandou <gilles@grandou.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "minicript/sha.h"

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h> /* BLKGETSIZE64 */
#endif

#ifdef __CYGWIN__
#include <sys/ioctl.h>
#include <cygwin/fs.h> /* BLKGETSIZE64 */
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <sys/disk.h> /* DIOCGMEDIASIZE */
#include <sys/sysctl.h>
#endif

#if defined(__APPLE__)
# include <sys/disk.h> /* DKIOCGETBLOCKCOUNT */
#endif


#ifdef HAS_BLKID
#include <blkid/blkid.h>
#endif

#include "version.h"
#include "bootimg.h"


enum command {
  none,
  help,
  info,
  extract,
  update,
  create
};


typedef struct
{
  unsigned     size;
  int          is_blkdev;

  char*        fname;
  char*        config_fname;
  char*        kernel_fname;
  char*        ramdisk_fname;
  char*        second_fname;
  char*        devtree_fname;

  FILE*        stream;

  boot_img_hdr header;

  char*        kernel;
  char*        ramdisk;
  char*        second;
  char*        devtree;
} t_abootimg;

#define MAX_CONF_LEN    4096
char config_args[MAX_CONF_LEN] = "";

unsigned padding_size(unsigned image_size, unsigned page_size) {
    unsigned delta = image_size & (page_size - 1);
    if(delta == 0)
        return 0;
    return page_size - delta;
}

void abort_perror(const char* str)
{
  perror(str);
  exit(errno);
}

void abort_printf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
  exit(1);
}


int blkgetsize(int fd, unsigned long long *pbsize)
{
# if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
  return ioctl(fd, DIOCGMEDIASIZE, pbsize);
# elif defined(__APPLE__)
  return ioctl(fd, DKIOCGETBLOCKCOUNT, pbsize);
# elif defined(__NetBSD__)
  // does a suitable ioctl exist?
  // return (ioctl(fd, DIOCGDINFO, &label) == -1);
  return 1;
# elif defined(__linux__) || defined(__CYGWIN__)
  return ioctl(fd, BLKGETSIZE64, pbsize);
# elif defined(__GNU__)
  // does a suitable ioctl for HURD exist?
  return 1;
# else
  return 1;
# endif

}

void print_usage(void)
{
  printf (
 " abootimg - manipulate Android Boot Images.\n"
 " (c) 2010-2011 Gilles Grandou <gilles@grandou.net>\n"
 " " VERSION_STR "\n"
 "\n"
 " abootimg [-h]\n"
 "\n"
 "      print usage\n"
 "\n"
 " abootimg -i <bootimg>\n"
 "\n"
 "      print boot image information\n"
 "\n"
 " abootimg -x <bootimg> [<bootimg.cfg> [<kernel> [<ramdisk> [<secondstage> [<device tree>]]]]]\n"
 "\n"
 "      extract objects from boot image:\n"
 "      - config file (default name bootimg.cfg)\n"
 "      - kernel image (default name zImage)\n"
 "      - ramdisk image (default name initrd.img)\n"
 "      - second stage image (default name stage2.img)\n"
 "      - device tree image (default name dt.img)\n"
 "\n"
 " abootimg -u <bootimg> [-c \"param=value\"] [-f <bootimg.cfg>] [-k <kernel>] [-r <ramdisk>] [-s <secondstage>] [-t <device tree>]\n"
 "\n"
 "      update a current boot image with objects given in command line\n"
 "      - header informations given in arguments (several can be provided)\n"
 "      - header informations given in config file\n"
 "      - kernel image\n"
 "      - ramdisk image\n"
 "      - second stage image\n"
 "      - device tree image\n"
 "\n"
 "      bootimg has to be valid Android Boot Image, or the update will abort.\n"
 "\n"
 " abootimg --create <bootimg> [-c \"param=value\"] [-f <bootimg.cfg>] -k <kernel> -r <ramdisk> [-s <secondstage>] [-t <device tree>]\n"
 "\n"
 "      create a new image from scratch.\n"
 "      if the boot image file is a block device, sanity check will be performed to avoid overwriting a existing\n"
 "      filesystem.\n"
 "\n"
 "      argurments are the same than for -u.\n"
 "      kernel and ramdisk are mandatory.\n"
 "\n"
    );
}


enum command parse_args(int argc, char** argv, t_abootimg* img)
{
  enum command cmd = none;
  int i;

  if (argc<2)
    return none;

  if (!strcmp(argv[1], "-h")) {
    return help;
  }
  else if (!strcmp(argv[1], "-i")) {
    cmd=info;
  }
  else if (!strcmp(argv[1], "-x")) {
    cmd=extract;
  }
  else if (!strcmp(argv[1], "-u")) {
    cmd=update;
  }
  else if (!strcmp(argv[1], "--create")) {
    cmd=create;
  }
  else
    return none;

  switch(cmd) {
    case none:
    case help:
	    break;

    case info:
      if (argc != 3)
        return none;
      img->fname = argv[2];
      break;
      
    case extract:
      if ((argc < 3) || (argc > 7))
        return none;
      img->fname = argv[2];
      if (argc >= 4)
        img->config_fname = argv[3];
      if (argc >= 5)
        img->kernel_fname = argv[4];
      if (argc >= 6)
        img->ramdisk_fname = argv[5];
      if (argc >= 7)
        img->second_fname = argv[6];
      if (argc >= 8)
        img->devtree_fname = argv[7];
      break;

    case update:
    case create:
      if (argc < 3)
        return none;
      img->fname = argv[2];
      img->config_fname = NULL;
      img->kernel_fname = NULL;
      img->ramdisk_fname = NULL;
      img->second_fname = NULL;
      img->devtree_fname = NULL;
      for(i=3; i<argc; i++) {
        if (!strcmp(argv[i], "-c")) {
          if (++i >= argc)
            return none;
          unsigned len = strlen(argv[i]);
          if (strlen(config_args)+len+1 >= MAX_CONF_LEN)
            abort_printf("too many config parameters.\n");
          strcat(config_args, argv[i]);
          strcat(config_args, "\n");
        }
        else if (!strcmp(argv[i], "-f")) {
          if (++i >= argc)
            return none;
          img->config_fname = argv[i];
        }
        else if (!strcmp(argv[i], "-k")) {
          if (++i >= argc)
            return none;
          img->kernel_fname = argv[i];
        }
        else if (!strcmp(argv[i], "-r")) {
          if (++i >= argc)
            return none;
          img->ramdisk_fname = argv[i];
        }
        else if (!strcmp(argv[i], "-s")) {
          if (++i >= argc)
            return none;
          img->second_fname = argv[i];
        }
        else if (!strcmp(argv[i], "-t")) {
          if (++i >= argc)
            return none;
          img->devtree_fname = argv[i];
        }
        else
          return none;
      }
      break;
  }
  
  return cmd;
}



int check_boot_img_header(t_abootimg* img)
{
  if (strncmp((char*)(img->header.magic), BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
    fprintf(stderr, "%s: no Android Magic Value\n", img->fname);
    return 1;
  }

  if (!(img->header.kernel_size)) {
    fprintf(stderr, "%s: kernel size is null\n", img->fname);
    return 1;
  }

  if (!(img->header.ramdisk_size)) {
    fprintf(stderr, "%s: ramdisk size is null\n", img->fname);
    return 1;
  }

  unsigned page_size = img->header.page_size;
  if (!page_size) {
    fprintf(stderr, "%s: Image page size is null\n", img->fname);
    return 1;
  }

  if (!(img->header.dt_size)) {
    fprintf(stderr, "%s: device tree is null\n", img->fname);
  }

//  if (!(img->header.unused)) {
//    fprintf(stderr, "%s: unused is null\n", img->fname);
//  }

  if (!(img->header.name)) {
    fprintf(stderr, "%s: name is null\n", img->fname);
  }

  if (!(img->header.cmdline)) {
    fprintf(stderr, "%s: cmdline is null\n", img->fname);
  }

  unsigned n = (img->header.kernel_size + page_size - 1) / page_size;
  unsigned m = (img->header.ramdisk_size + page_size - 1) / page_size;
  unsigned o = (img->header.second_size + page_size - 1) / page_size;
  unsigned p = (img->header.dt_size + page_size - 1) / page_size;

  unsigned total_size = (1+n+m+o+p)*page_size;

  if (total_size > img->size) {
    fprintf(stderr, "%s: sizes mismatches in boot image\n", img->fname);
    return 1;
  }

  return 0;
}



void check_if_block_device(t_abootimg* img)
{
  struct stat st;

  if (stat(img->fname, &st))
    if (errno != ENOENT) {
      printf("errno=%d\n", errno);
      abort_perror(img->fname);
    }

#ifdef HAS_BLKID
  if (S_ISBLK(st.st_mode)) {
    img->is_blkdev = 1;

    char* type = blkid_get_tag_value(NULL, "TYPE", img->fname);
    if (type)
      abort_printf("%s: refuse to write on a valid partition type (%s)\n", img->fname, type);

    int fd = open(img->fname, O_RDONLY);
    if (fd == -1)
      abort_perror(img->fname);
    
    unsigned long long bsize = 0;
    if (blkgetsize(fd, &bsize))
      abort_perror(img->fname);
    img->size = bsize;

    close(fd);
  }
#endif
}



void open_bootimg(t_abootimg* img, char* mode)
{
  img->stream = fopen(img->fname, mode);
  if (!img->stream)
    abort_perror(img->fname);
}



void read_header(t_abootimg* img)
{
  size_t rb = fread(&img->header, sizeof(boot_img_hdr), 1, img->stream);
  if ((rb!=1) || ferror(img->stream))
    abort_perror(img->fname);
  else if (feof(img->stream))
    abort_printf("%s: cannot read image header\n", img->fname);

  struct stat s;
  int fd = fileno(img->stream);
  if (fstat(fd, &s))
    abort_perror(img->fname);

  if (S_ISBLK(s.st_mode)) {
    unsigned long long bsize = 0;

    if (blkgetsize(fd, &bsize))
      abort_perror(img->fname);
    img->size = bsize;
    img->is_blkdev = 1;
  }
  else {
    img->size = s.st_size;
    img->is_blkdev = 0;
  }

  if (check_boot_img_header(img))
    abort_printf("%s: not a valid Android Boot Image.\n", img->fname);
}



void update_header_entry(t_abootimg* img, char* cmd)
{
  char *p;
  char *token;
  char *endtoken;
  char *value;

  p = strchr(cmd, '\n');
  if (p)
    *p  = '\0';

  p = cmd;
  p += strspn(p, " \t");
  token = p;
  
  p += strcspn(p, " =\t");
  endtoken = p;
  p += strspn(p, " \t");

  if (*p++ != '=')
    goto err;

  p += strspn(p, " \t");
  value = p;

  *endtoken = '\0';

  unsigned valuenum = strtoul(value, NULL, 0);
  
  if (!strcmp(token, "cmdline")) {
    unsigned len = strlen(value);
    if (len >= BOOT_ARGS_SIZE) 
      abort_printf("cmdline length (%d) is too long (max %d)", len, BOOT_ARGS_SIZE-1);
    memset(img->header.cmdline, 0, BOOT_ARGS_SIZE);
    strcpy((char*)(img->header.cmdline), value);
  }
  else if (!strcmp(token, "extra_cmdline")) {
    unsigned len = strlen(value);
    if (len >= BOOT_ARGS_SIZE) 
      abort_printf("extra_cmdline length (%d) is too long (max %d)", len, BOOT_EXTRA_ARGS_SIZE-1);
    memset(img->header.extra_cmdline, 0, BOOT_EXTRA_ARGS_SIZE);
    strcpy((char*)(img->header.extra_cmdline), value);
  }
  else if (!strncmp(token, "name", 4)) {
    strncpy((char*)(img->header.name), value, BOOT_NAME_SIZE);
    img->header.name[BOOT_NAME_SIZE-1] = '\0';
  }
  else if (!strncmp(token, "bootsize", 8)) {
    if (img->is_blkdev && (img->size != valuenum))
      abort_printf("%s: cannot change Boot Image size for a block device\n", img->fname);
    img->size = valuenum;
  }
  else if (!strncmp(token, "pagesize", 8)) {
    img->header.page_size = valuenum;
  }
  else if (!strncmp(token, "kerneladdr", 10)) {
    img->header.kernel_addr = valuenum;
  }
  else if (!strncmp(token, "ramdiskaddr", 11)) {
    img->header.ramdisk_addr = valuenum;
  }
  else if (!strncmp(token, "secondaddr", 10)) {
    img->header.second_addr = valuenum;
  }
  else if (!strncmp(token, "tagsaddr", 8)) {
    img->header.tags_addr = valuenum;
  }
  else if (!strncmp(token, "devtree", 7)) {
    img->header.dt_size = valuenum;
  }
  else
    goto err;
  return;

err:
  abort_printf("%s: bad config entry\n", token);
}


void update_header(t_abootimg* img)
{
  if (img->config_fname) {
    FILE* config_file = fopen(img->config_fname, "r");
    if (!config_file)
      abort_perror(img->config_fname);

    printf("reading config file %s\n", img->config_fname);

    char* line = NULL;
    size_t len = 0;
    int read;

    while ((read = getline(&line, &len, config_file)) != -1) {
      update_header_entry(img, line);
      free(line);
      line = NULL;
    }
    if (ferror(config_file))
      abort_perror(img->config_fname);
  }

  unsigned len = strlen(config_args);
  if (len) {
    FILE* config_file = fmemopen(config_args, len, "r");
    if  (!config_file)
      abort_perror("-c args");

    printf("reading config args\n");

    char* line = NULL;
    size_t len = 0;
    int read;

    while ((read = getline(&line, &len, config_file)) != -1) {
      update_header_entry(img, line);
      free(line);
      line = NULL;
    }
    if (ferror(config_file))
      abort_perror("-c args");
  }
}

unsigned slurp_file(const char *fname, char** data)
{
  FILE* stream = fopen(fname, "r");
  if (!stream)
    abort_perror(fname);

  struct stat st;
  if (fstat(fileno(stream), &st))
    abort_perror(fname);
  unsigned size = st.st_size;

  char* d = malloc(size);
  if (!d)
    abort_perror("malloc");

  size_t rb = fread(d, size, 1, stream);
  if ((rb!=1) || ferror(stream))
    abort_perror(fname);
  else if (feof(stream))
    abort_printf("%s: cannot read\n", fname);

  *data = d;

  fclose(stream);
  return size;
}

char * slurp_section(t_abootimg *img, unsigned offset, unsigned size) {
  char* r = malloc(size);
  if (!r)
    abort_perror("");

  if (fseek(img->stream, offset, SEEK_SET))
    abort_perror(img->fname);

  size_t rb = fread(r, size, 1, img->stream);
  if ((rb!=1) || ferror(img->stream))
    abort_perror(img->fname);
  else if (feof(img->stream))
    abort_printf("%s: cannot read section\n", img->fname);

  return r;
}

void update_images(t_abootimg *img)
{
  unsigned page_size = img->header.page_size;
  unsigned ksize = img->header.kernel_size;
  unsigned rsize = img->header.ramdisk_size;
  unsigned ssize = img->header.second_size;
  unsigned dtsize = img->header.dt_size;

  if (!page_size)
    abort_printf("%s: Image page size is null\n", img->fname);

  unsigned koffset = page_size;

  if (img->kernel_fname) {
    printf("reading kernel from %s\n", img->kernel_fname);
    img->header.kernel_size = slurp_file(img->kernel_fname, &img->kernel);
  } else {
    img->kernel = slurp_section(img, koffset, img->header.kernel_size);
  }

  unsigned n = (ksize + page_size - 1) / page_size;
  unsigned roffset = (1+n)*page_size;

  if (img->ramdisk_fname) {
    printf("reading ramdisk from %s\n", img->ramdisk_fname);
    rsize = slurp_file(img->ramdisk_fname, &img->ramdisk);
    img->header.ramdisk_size = rsize;
  } else {
    img->ramdisk = slurp_section(img, roffset, rsize);
  }

  unsigned m = (rsize + page_size - 1) / page_size;
  unsigned soffset = (1+n+m)*page_size;

  if (img->second_fname) {
    printf("reading second stage from %s\n", img->second_fname);
    ssize = slurp_file(img->second_fname, &img->second);
    img->header.second_size = ssize;
  } else if (img->header.second_size) {
    img->second = slurp_section(img, soffset, ssize);
  }

  unsigned o = (ssize + page_size - 1) / page_size;
  unsigned p = (dtsize + page_size - 1) / page_size;
  unsigned dtoffset = (1+n+m+o)*page_size;

  if (img->devtree_fname) {
    printf("reading device tree from %s\n", img->devtree_fname);
    dtsize = slurp_file(img->devtree_fname, &img->devtree);
    img->header.dt_size = dtsize;
  } else if (img->header.dt_size) {
    img->devtree = slurp_section(img, dtoffset, dtsize);
  }

  n = (img->header.kernel_size + page_size - 1) / page_size;
  m = (img->header.ramdisk_size + page_size - 1) / page_size;
  o = (img->header.second_size + page_size - 1) / page_size;
  p = (img->header.dt_size + page_size - 1) / page_size;
  unsigned total_size = (1+n+m+o+p)*page_size;

  if (!img->size)
    img->size = total_size;
  else if (total_size > img->size)
    abort_printf("%s: updated is too big for the Boot Image (%u vs %u bytes)\n", img->fname, total_size, img->size);
}



void write_bootimg(t_abootimg* img)
{
  unsigned psize;
  char* padding;
  SHA_CTX ctx;

  printf ("Writing Boot Image %s\n", img->fname);

  psize = img->header.page_size;
  padding = calloc(psize, 1);
  if (!padding)
    abort_perror("");

  unsigned n = (img->header.kernel_size + psize - 1) / psize;
  unsigned m = (img->header.ramdisk_size + psize - 1) / psize;
  unsigned o = (img->header.second_size + psize - 1) / psize;

  if (fseek(img->stream, 0, SEEK_SET))
    abort_perror(img->fname);
  
  SHA_init(&ctx);
  SHA_update(&ctx, img->kernel, img->header.kernel_size);
  SHA_update(&ctx, &img->header.kernel_size, sizeof(img->header.kernel_size));
  SHA_update(&ctx, img->ramdisk, img->header.ramdisk_size);
  SHA_update(&ctx, &img->header.ramdisk_size, sizeof(img->header.ramdisk_size));
  SHA_update(&ctx, img->second, img->header.second_size);
  SHA_update(&ctx, &img->header.second_size, sizeof(img->header.second_size));

  if (img->devtree) {
    SHA_update(&ctx, img->devtree, img->header.dt_size);
    SHA_update(&ctx, &img->header.dt_size, sizeof(img->header.dt_size));
  }

  const unsigned char* sha = SHA_final(&ctx);
  memcpy(img->header.id, sha, SHA_DIGEST_SIZE > sizeof(img->header.id) ? sizeof(img->header.id) : SHA_DIGEST_SIZE);

  fwrite(&img->header, sizeof(img->header), 1, img->stream);
  if (ferror(img->stream))
    abort_perror(img->fname);

  fwrite(padding, psize - sizeof(img->header), 1, img->stream);
  if (ferror(img->stream))
    abort_perror(img->fname);

  if (img->kernel) {
    fwrite(img->kernel, img->header.kernel_size, 1, img->stream);
    if (ferror(img->stream))
      abort_perror(img->fname);

    unsigned pad_size = padding_size(img->header.kernel_size, psize);
    if(pad_size > 0) {
        fwrite(padding, pad_size, 1, img->stream);
        if (ferror(img->stream))
          abort_perror(img->fname);
    }
  }

  if (img->ramdisk) {
    if (fseek(img->stream, (1+n)*psize, SEEK_SET))
      abort_perror(img->fname);

    fwrite(img->ramdisk, img->header.ramdisk_size, 1, img->stream);
    if (ferror(img->stream))
      abort_perror(img->fname);

    unsigned pad_size = padding_size(img->header.ramdisk_size, psize);
    if(pad_size > 0) {
        fwrite(padding, pad_size, 1, img->stream);
        if (ferror(img->stream))
          abort_perror(img->fname);
    }
  }

  if (img->header.second_size) {
    if (fseek(img->stream, (1+n+m)*psize, SEEK_SET))
      abort_perror(img->fname);

    fwrite(img->second, img->header.second_size, 1, img->stream);
    if (ferror(img->stream))
      abort_perror(img->fname);

    unsigned pad_size = padding_size(img->header.second_size, psize);
    if(pad_size > 0) {
        fwrite(padding, pad_size, 1, img->stream);
        if (ferror(img->stream))
          abort_perror(img->fname);
    }
  }
  
  if (img->header.dt_size) {
    if (fseek(img->stream, (1+n+m+o)*psize, SEEK_SET))
      abort_perror(img->fname);

    fwrite(img->devtree, img->header.dt_size, 1, img->stream);
    if (ferror(img->stream))
      abort_perror(img->fname);

    unsigned pad_size = padding_size(img->header.dt_size, psize);
    if(pad_size > 0) {
        fwrite(padding, pad_size, 1, img->stream);
        if (ferror(img->stream))
          abort_perror(img->fname);
    }
  }

  ftruncate(fileno(img->stream), img->size);

  free(padding);
}



void print_bootimg_info(t_abootimg* img)
{
  printf ("\nAndroid Boot Image Info:\n\n");

  printf ("* file name = %s %s\n\n", img->fname, img->is_blkdev ? "[block device]":"");

  printf ("* image size = %u bytes (%.2f MB)\n", img->size, (double)img->size/0x100000);
  printf ("  page size  = %u bytes\n\n", img->header.page_size);

  printf ("* Boot Name = \"%s\"\n\n", img->header.name);

  unsigned kernel_size = img->header.kernel_size;
  unsigned ramdisk_size = img->header.ramdisk_size;
  unsigned second_size = img->header.second_size;
  unsigned devtree_size = img->header.dt_size;

  printf ("* kernel size       = %u bytes (%.2f MB)\n", kernel_size, (double)kernel_size/0x100000);
  printf ("  ramdisk size      = %u bytes (%.2f MB)\n", ramdisk_size, (double)ramdisk_size/0x100000);
  if (second_size)
    printf ("  second stage size = %u bytes (%.2f MB)\n", ramdisk_size, (double)ramdisk_size/0x100000);
  if(devtree_size)
    printf ("  device tree size  = %u bytes (%.2f MB)\n", devtree_size, (double)devtree_size/0x100000);
 
  printf ("\n* load addresses:\n");
  printf ("  kernel:       0x%08x\n", img->header.kernel_addr);
  printf ("  ramdisk:      0x%08x\n", img->header.ramdisk_addr);
  if (second_size)
    printf ("  second stage: 0x%08x\n", img->header.second_addr);
  if (devtree_size)
    printf ("  device tree:  0x%08x\n", img->header.dt_size);
  printf ("  tags:         0x%08x\n\n", img->header.tags_addr);

  if (img->header.cmdline[0])
    printf ("* cmdline = %s\n\n", img->header.cmdline);
  else
    printf ("* empty cmdline\n");

  if (img->header.extra_cmdline[0])
    printf ("* extra cmdline = %s\n\n", img->header.extra_cmdline);
  else
    printf ("* empty extra cmdline\n");

  printf ("* id = ");
  int i;
  for (i=0; i<8; i++)
    printf ("0x%08x ", img->header.id[i]);
  printf ("\n\n");
}



void write_bootimg_config(t_abootimg* img)
{
  printf ("writing boot image config in %s\n", img->config_fname);

  FILE* config_file = fopen(img->config_fname, "w");
  if (!config_file)
    abort_perror(img->config_fname);

  fprintf(config_file, "bootsize = 0x%x\n", img->size);
  fprintf(config_file, "pagesize = 0x%x\n", img->header.page_size);

  fprintf(config_file, "kerneladdr = 0x%x\n", img->header.kernel_addr);
  fprintf(config_file, "ramdiskaddr = 0x%x\n", img->header.ramdisk_addr);
  fprintf(config_file, "secondaddr = 0x%x\n", img->header.second_addr);
  fprintf(config_file, "devtree = 0x%x\n", img->header.dt_size);
  fprintf(config_file, "tagsaddr = 0x%x\n", img->header.tags_addr);

  fprintf(config_file, "name = %s\n", img->header.name);
  fprintf(config_file, "cmdline = %s\n", img->header.cmdline);
  fprintf(config_file, "extra_cmdline = %s\n", img->header.extra_cmdline);
  
  fclose(config_file);
}



void extract_kernel(t_abootimg* img)
{
  unsigned psize = img->header.page_size;
  unsigned ksize = img->header.kernel_size;

  unsigned koffset = psize;

  printf ("extracting kernel in %s\n", img->kernel_fname);

  void* k = malloc(ksize);
  if (!k)
    abort_perror(NULL);

  if (fseek(img->stream, koffset, SEEK_SET))
    abort_perror(img->fname);

  size_t rb = fread(k, ksize, 1, img->stream);
  if ((rb!=1) || ferror(img->stream))
    abort_perror(img->fname);
 
  FILE* kernel_file = fopen(img->kernel_fname, "w");
  if (!kernel_file)
    abort_perror(img->kernel_fname);

  fwrite(k, ksize, 1, kernel_file);
  if (ferror(kernel_file))
    abort_perror(img->kernel_fname);

  fclose(kernel_file);
  free(k);
}



void extract_ramdisk(t_abootimg* img)
{
  unsigned psize = img->header.page_size;
  unsigned ksize = img->header.kernel_size + padding_size(img->header.kernel_size, psize);
  unsigned rsize = img->header.ramdisk_size;

  unsigned roffset = psize + ksize;

  printf ("extracting ramdisk in %s\n", img->ramdisk_fname);

  void* r = malloc(rsize);
  if (!r) 
    abort_perror(NULL);

  if (fseek(img->stream, roffset, SEEK_SET))
    abort_perror(img->fname);

  size_t rb = fread(r, rsize, 1, img->stream);
  if ((rb!=1) || ferror(img->stream))
    abort_perror(img->fname);
 
  FILE* ramdisk_file = fopen(img->ramdisk_fname, "w");
  if (!ramdisk_file)
    abort_perror(img->ramdisk_fname);

  fwrite(r, rsize, 1, ramdisk_file);
  if (ferror(ramdisk_file))
    abort_perror(img->ramdisk_fname);

  fclose(ramdisk_file);
  free(r);
}



void extract_second(t_abootimg* img)
{
  unsigned psize = img->header.page_size;
  unsigned ksize = img->header.kernel_size + padding_size(img->header.kernel_size, psize);
  unsigned rsize = img->header.ramdisk_size + padding_size(img->header.ramdisk_size, psize);
  unsigned ssize = img->header.second_size;

  if (!ssize) // Second Stage not present
    return;

  unsigned n = (rsize + ksize + psize - 1) / psize;
  unsigned soffset = (1+n)*psize;

  printf ("extracting second stage image in %s\n", img->second_fname);

  void* s = malloc(ssize);
  if (!s)
    abort_perror(NULL);

  if (fseek(img->stream, soffset, SEEK_SET))
    abort_perror(img->fname);

  size_t rb = fread(s, ssize, 1, img->stream);
  if ((rb!=1) || ferror(img->stream))
    abort_perror(img->fname);
 
  FILE* second_file = fopen(img->second_fname, "w");
  if (!second_file)
    abort_perror(img->second_fname);

  fwrite(s, ssize, 1, second_file);
  if (ferror(second_file))
    abort_perror(img->second_fname);

  fclose(second_file);
  free(s);
}

void extract_devtree(t_abootimg* img)
{
  unsigned psize = img->header.page_size;
  unsigned ksize = img->header.kernel_size + padding_size(img->header.kernel_size, psize);
  unsigned rsize = img->header.ramdisk_size + padding_size(img->header.ramdisk_size, psize);
  unsigned ssize = img->header.second_size + padding_size(img->header.second_size, psize);
  unsigned dtsize = img->header.dt_size;

  if (!dtsize) // Device tree not present
    return;

  unsigned n = (ssize + rsize + ksize + psize - 1) / psize;
  unsigned dtoffset = (1+n)*psize;

  printf ("extracting device tree image in %s\n", img->devtree_fname);

  void* dt = malloc(ksize);
  if (!dt)
    abort_perror(NULL);

  if (fseek(img->stream, dtoffset, SEEK_SET))
    abort_perror(img->fname);

  size_t rb = fread(dt, dtsize, 1, img->stream);
  if ((rb!=1) || ferror(img->stream))
    abort_perror(img->fname);
 
  FILE* devtree_file = fopen(img->devtree_fname, "w");
  if (!devtree_file)
    abort_perror(img->devtree_fname);

  fwrite(dt, dtsize, 1, devtree_file);
  if (ferror(devtree_file))
    abort_perror(img->devtree_fname);

  fclose(devtree_file);
  free(dt);
}


t_abootimg* new_bootimg()
{
  t_abootimg* img;

  img = calloc(sizeof(t_abootimg), 1);
  if (!img)
    abort_perror(NULL);

  img->config_fname = "bootimg.cfg";
  img->kernel_fname = "zImage";
  img->ramdisk_fname = "initrd.img";
  img->second_fname = "stage2.img";
  img->devtree_fname = "dt.img";

  memcpy(img->header.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE);
  img->header.page_size = 2048;  // a sensible default page size

  return img;
}


int main(int argc, char** argv)
{
  t_abootimg* bootimg = new_bootimg();

  switch(parse_args(argc, argv, bootimg))
  {
    case none:
      printf("error - bad arguments\n\n");
      print_usage();
      break;

    case help:
      print_usage();
      break;

    case info:
      open_bootimg(bootimg, "r");
      read_header(bootimg);
      print_bootimg_info(bootimg);
      break;

    case extract:
      open_bootimg(bootimg, "r");
      read_header(bootimg);
      write_bootimg_config(bootimg);
      extract_kernel(bootimg);
      extract_ramdisk(bootimg);
      extract_second(bootimg);
      extract_devtree(bootimg);
      break;
    
    case update:
      open_bootimg(bootimg, "r+");
      read_header(bootimg);
      update_header(bootimg);
      update_images(bootimg);
      write_bootimg(bootimg);
      break;

    case create:
      if (!bootimg->kernel_fname || !bootimg->ramdisk_fname) {
        print_usage();
        break;
      }
      check_if_block_device(bootimg);
      open_bootimg(bootimg, "w");
      update_header(bootimg);
      update_images(bootimg);
      if (check_boot_img_header(bootimg))
        abort_printf("%s: Sanity cheks failed", bootimg->fname);
      write_bootimg(bootimg);
      break;
  }

  return 0;
}

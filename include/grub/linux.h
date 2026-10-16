#include <grub/file.h>
#include <grub/symbol.h>

/* Optional Linux argument transformations. A registering module owns the
   transformed vector and must unregister before unloading. */
enum grub_linux_arg_stage
{
  GRUB_LINUX_ARG_KERNEL,
  GRUB_LINUX_ARG_INITRD,
  GRUB_LINUX_ARG_KERNEL16
};

typedef grub_err_t (*grub_linux_args_hook_t)
  (enum grub_linux_arg_stage stage, int argc, char *argv[],
   int *out_argc, char ***out_argv);
typedef void (*grub_linux_args_free_hook_t) (int argc, char **argv);

void EXPORT_FUNC(grub_linux_register_args_hook)
  (grub_linux_args_hook_t hook, grub_linux_args_free_hook_t release);
void EXPORT_FUNC(grub_linux_unregister_args_hook)
  (grub_linux_args_hook_t hook);
grub_err_t EXPORT_FUNC(grub_linux_prepare_args)
  (enum grub_linux_arg_stage stage, int argc, char *argv[],
   int *out_argc, char ***out_argv);
void EXPORT_FUNC(grub_linux_release_args)
  (char *original_argv[], int argc, char **argv);

struct grub_linux_initrd_component;

struct grub_linux_initrd_context
{
  int nfiles;
  struct grub_linux_initrd_component *components;
  grub_size_t size;
};

grub_err_t
grub_initrd_init (int argc, char *argv[],
		  struct grub_linux_initrd_context *ctx);

grub_size_t
grub_get_initrd_size (struct grub_linux_initrd_context *ctx);

void
grub_initrd_close (struct grub_linux_initrd_context *initrd_ctx);

grub_err_t
grub_initrd_load (struct grub_linux_initrd_context *initrd_ctx,
		  void *target);

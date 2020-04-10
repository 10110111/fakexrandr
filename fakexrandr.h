/*
	The management script uses this symbol to identify the fake libXrandr version
*/
int _is_fake_xrandr = 1;

/*
	We use this XID modifier to flag outputs and CRTCs as
	fake by adding a counter in the first few bits:
	 · xid & ~XID_SPLIT_MASK is the xid of the original output
	 · xid >> XID_SPLIT_SHIFT is the counter identifying a virtual, split screen

	On the choice: A typical XID is of the form
	 client_id | (xid_mask & arbitrary value),
	according to the documentation of the X-Resource extension. On my system,
	 xid_mask = 0x001FFFFF
	and client_id == 0 for all reources mentioned in the RandR protocol.
	All we need to do is to choose XID_SPLIT_MASK such that
	XID_SPLIT_MASK & xid_mask == 0.
*/
#define XID_SPLIT_SHIFT 21
#define XID_SPLIT_MASK  0x7FE00000

/*
    Routines used by libXrandr and libxcb-randr for loading saved configuration
*/

#include <sys/stat.h>

static char *config_file;
static int config_file_fd;
static size_t config_file_size;

static void close_configuration() {
	munmap(config_file, config_file_size);
	close(config_file_fd);
	config_file = NULL;
}

static int open_configuration() {
	// Load the configuration from ${XDG_CONFIG_HOME:-$HOME/.config}/fakexrandr.bin
	if(config_file) {
		close_configuration();
	}

	char *config_dir = getenv("XDG_CONFIG_HOME");
	if(!config_dir) {
		char *home_dir = getenv("HOME");
		if(!home_dir) {
			return 1;
		}
		config_dir = (char*)alloca(512);
		if(snprintf(config_dir, 512, "%s/.config", home_dir) >= 512) {
			return 1;
		}
	}

	char config_file_path[512];
	if(snprintf(config_file_path, 512, "%s/fakexrandr.bin", config_dir) >= 512) {
		return 1;
	}
	if(access(config_file_path, R_OK)) {
		return 1;
	}

	config_file_fd = open(config_file_path, O_RDONLY);
	if(config_file_fd < 0) {
		perror("fakexrandr/open()");
		return 1;
	}
	struct stat config_stat;
	fstat(config_file_fd, &config_stat);
	config_file_size = config_stat.st_size;
	config_file = (char*)mmap(NULL, config_file_size, PROT_READ, MAP_SHARED, config_file_fd, 0);
	if(config_file == MAP_FAILED) {
		perror("fakexrandr/mmap()");
		config_file = NULL;
		close(config_file_fd);
		return 1;
	}

	return 0;
}

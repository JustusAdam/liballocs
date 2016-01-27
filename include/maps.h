#include <string.h>
#include <unistd.h>

inline ssize_t get_a_line(char *buf, size_t size, int fd)
{
	if (size == 0) return -1; // now size is at least 1
	
	// read some stuff, at most `size - 1' bytes (we're going to add a null), into the buffer
	ssize_t bytes_read = read(fd, buf, size - 1);
	
	// if we got EOF and read zero bytes, return -1
	if (bytes_read == 0) return -1;
	
	// did we get enough that we have a whole line?
	char *found = memchr(buf, '\n', bytes_read);
	// if so, rewind the file to just after the newline
	if (found)
	{
		size_t end_of_newline_displacement = (found - buf) + 1;
		(void) lseek(fd, 
				end_of_newline_displacement - bytes_read /* i.e. negative if we read more */,
				SEEK_CUR);
		buf[end_of_newline_displacement] = '\0';
		return end_of_newline_displacement;
	}
	else
	{
		/* We didn't read enough. But that should only be because of EOF of error.
		 * So just return whatever we got. */
		buf[bytes_read] = '\0';
		return bytes_read;
	}
}
struct proc_entry
{
	unsigned long first, second;
	char r, w, x, p;
	unsigned offset;
	unsigned devmaj, devmin;
	unsigned inode;
	char rest[4096];
};
typedef int maps_cb_t(struct proc_entry *ent, char *linebuf, size_t bufsz, void *arg);

inline int for_each_maps_entry(int fd, char *linebuf, size_t bufsz, struct proc_entry *entry_buf, 
		maps_cb_t *cb, void *arg)
{
	#define NUM_FIELDS 11
	while (get_a_line(linebuf, bufsz, fd) != -1)
	{
		entry_buf->rest[0] = '\0';
		int fields_read = sscanf(linebuf, 
			"%lx-%lx %c%c%c%c %8x %2x:%2x %d %4095[\x01-\x09\x0b-\xff]\n",
			&entry_buf->first, &entry_buf->second, &entry_buf->r, &entry_buf->w, &entry_buf->x, 
			&entry_buf->p, &entry_buf->offset, &entry_buf->devmaj, &entry_buf->devmin, 
			&entry_buf->inode, entry_buf->rest);

		assert(fields_read >= (NUM_FIELDS-1)); // we might not get a "rest"
		
		int ret = cb(entry_buf, linebuf, bufsz, arg);
		if (ret) return ret;
	}
	return 0;
	#undef NUM_FIELDS
}

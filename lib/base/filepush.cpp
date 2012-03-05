#include <lib/base/filepush.h>
#include <lib/base/eerror.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#if defined(__sh__) // this allows filesystem tasks to be prioritised
#include <sys/vfs.h>

#define USBDEVICE_SUPER_MAGIC 0x9fa2 
#define EXT2_SUPER_MAGIC      0xEF53 
#define EXT3_SUPER_MAGIC      0xEF53 
#define SMB_SUPER_MAGIC       0x517B 
#define NFS_SUPER_MAGIC       0x6969 
#define MSDOS_SUPER_MAGIC     0x4d44            /* MD */
#endif
#if defined(__sh__) // nits shm hack to behavior of e2 on the fly
#include "include/shmE2.h"
extern char *shm;
#endif

#define PVR_COMMIT 1

//FILE *f = fopen("/log.ts", "wb");

eFilePushThread::eFilePushThread(int io_prio_class, int io_prio_level, int blocksize)
	:prio_class(io_prio_class), prio(io_prio_level), m_messagepump(eApp, 0)
{
	m_stop = 0;
	m_sg = 0;
	m_send_pvr_commit = 0;
	m_stream_mode = 0;
	m_blocksize = blocksize;
	flush();
	enablePVRCommit(0);
	CONNECT(m_messagepump.recv_msg, eFilePushThread::recvEvent);

// vvv Initialize with 0, because otherwise playback may stop
// (In case of playback eFilePushThread::start is not called)		
#if defined(__sh__)
	m_record_split_size = 0;
	m_record_split_type = 0;
#endif
// ^^^ Initialize with 0, because otherwise playback may stop

}

static void signal_handler(int x)
{
}

void eFilePushThread::thread()
{
	setIoPrio(prio_class, prio);

	off_t dest_pos = 0;
	size_t bytes_read = 0;
	
	off_t current_span_offset = 0;
	size_t current_span_remaining = 0;
	
	size_t written_since_last_sync = 0;

#if defined(__sh__) // used for filesplitting
	int file_count = 1;
	unsigned long long total_bytes_written = 0;
#endif

	eDebug("FILEPUSH THREAD START");
	
		/* we set the signal to not restart syscalls, so we can detect our signal. */
	struct sigaction act;
	act.sa_handler = signal_handler; // no, SIG_IGN doesn't do it. we want to receive the -EINTR
	act.sa_flags = 0;
	sigaction(SIGUSR1, &act, 0);
	
	hasStarted();

#if defined(__sh__) // opens video device for the reverse playback workaround
//Changes in this file are cause e2 doesnt tell the player to play reverse
//No idea how this is handeld in dm drivers
	int fd_video = open("/dev/dvb/adapter0/video0", O_RDONLY);
#endif
		/* m_stop must be evaluated after each syscall. */
		
// vvv Fix to ensure that event evtEOF is called at end of playbackl part 1/3
	bool already_empty=false;
// ^^^ Fix to ensure that event evtEOF is called at end of playbackl part 1/3
	while (!m_stop)
	{
			/* first try flushing the bufptr */
		if (m_buf_start != m_buf_end)
		{
				/* filterRecordData wants to work on multiples of blocksize.
				   if it returns a negative result, it means that this many bytes should be skipped
				   *in front* of the buffer. Then, it will be called again. with the newer, shorter buffer.
				   if filterRecordData wants to skip more data then currently available, it must do that internally.
				   Skipped bytes will also not be output.

				   if it returns a positive result, that means that only these many bytes should be used
				   in the buffer. 
				   
				   In either case, current_span_remaining is given as a reference and can be modified. (Of course it 
				   doesn't make sense to decrement it to a non-zero value unless you return 0 because that would just
				   skip some data). This is probably a very special application for fast-forward, where the current
				   span is to be cancelled after a complete iframe has been output.

				   we always call filterRecordData with our full buffer (otherwise we couldn't easily strip from the end)
				   
				   we filter data only once, of course, but it might not get immediately written.
				   that's what m_filter_end is for - it points to the start of the unfiltered data.
				*/
			
			int filter_res;
			
			do
			{
				filter_res = filterRecordData(m_buffer + m_filter_end, m_buf_end - m_filter_end, current_span_remaining);

				if (filter_res < 0)
				{
					eDebug("[eFilePushThread] filterRecordData re-syncs and skips %d bytes", -filter_res);
					m_buf_start = m_filter_end + -filter_res;  /* this will also drop unwritten data */
					ASSERT(m_buf_start <= m_buf_end); /* otherwise filterRecordData skipped more data than available. */
					continue; /* try again */
				}
				
					/* adjust end of buffer to strip dropped tail bytes */
				m_buf_end = m_filter_end + filter_res;
					/* mark data as filtered. */
				m_filter_end = m_buf_end;
			} while (0);
			
			ASSERT(m_filter_end == m_buf_end);
			
			if (m_buf_start == m_buf_end)
				continue;

				/* now write out data. it will be 'aligned' (according to filterRecordData). 
				   absolutely forbidden is to return EINTR and consume a non-aligned number of bytes. 
				*/
			int w = write(m_fd_dest, m_buffer + m_buf_start, m_buf_end - m_buf_start);
//			fwrite(m_buffer + m_buf_start, 1, m_buf_end - m_buf_start, f);
//			eDebug("wrote %d bytes", w);
			if (w <= 0)
			{
				if (errno == EINTR || errno == EAGAIN || errno == EBUSY)
					continue;
				eDebug("eFilePushThread WRITE ERROR");
				sendEvent(evtWriteError);
				break;
				// ... we would stop the thread
			}

			written_since_last_sync += w;

			if (written_since_last_sync >= 512*1024)
			{
				int toflush = written_since_last_sync > 2*1024*1024 ?
					2*1024*1024 : written_since_last_sync &~ 4095; // write max 2MB at once
				dest_pos = lseek(m_fd_dest, 0, SEEK_CUR);
				dest_pos -= toflush;
				posix_fadvise(m_fd_dest, dest_pos, toflush, POSIX_FADV_DONTNEED);
				written_since_last_sync -= toflush;
#if defined(__sh__) // splits files
				//split file only after sync
				if (m_record_split_size)
				{
					total_bytes_written += toflush;
					if(total_bytes_written > m_record_split_size)
					{
						char filename[255];

						close(m_fd_dest);
						total_bytes_written = 0;
						written_since_last_sync = 0;

						if(m_record_split_type)
							snprintf(filename, 255, "%s.%03d.ts", m_filename, file_count++);
						else
							snprintf(filename, 255, "%s.%03d", m_filename, file_count++);

						eDebug("split record file - Recording to %s...", filename);

						m_fd_dest = open(filename, m_flags, 0644);
						if (m_fd_dest == -1)
						{
							eDebug("split record file - can't open recording file!");
						}

						/* turn off kernel caching strategies */
						posix_fadvise(m_fd_dest, 0, 0, POSIX_FADV_RANDOM);
					}
				}
#endif
			}

//			printf("FILEPUSH: wrote %d bytes\n", w);
			m_buf_start += w;
			continue;
		}

			/* now fill our buffer. */
			
		if (m_sg && !current_span_remaining)
		{
#if defined (__sh__) // tells the player to play in reverse
#define VIDEO_DISCONTINUITY                   _IO('o',  84)
#define DVB_DISCONTINUITY_SKIP                0x01
#define DVB_DISCONTINUITY_CONTINUOUS_REVERSE  0x02
			if((m_sg->getSkipMode() != 0))
			{
				// inform the player about the jump in the stream data
				// this only works if the video device allows the discontinuity ioctl in read-only mode (patched)
				int param = DVB_DISCONTINUITY_SKIP; // | DVB_DISCONTINUITY_CONTINUOUS_REVERSE;
				int rc = ioctl(fd_video, VIDEO_DISCONTINUITY, (void*)param);
				//eDebug("VIDEO_DISCONTINUITY (fd %d, rc %d)", fd_video, rc);
			}
#endif
			m_sg->getNextSourceSpan(m_current_position, bytes_read, current_span_offset, current_span_remaining);
			ASSERT(!(current_span_remaining % m_blocksize));
			m_current_position = current_span_offset;
			bytes_read = 0;
		}

		size_t maxread = sizeof(m_buffer);
		
			/* if we have a source span, don't read past the end */
		if (m_sg && maxread > current_span_remaining)
			maxread = current_span_remaining;

			/* align to blocksize */
		maxread -= maxread % m_blocksize;

		m_buf_start = 0;
		m_filter_end = 0;
		m_buf_end = 0;

		if (maxread)
			m_buf_end = m_source->read(m_current_position, m_buffer, maxread);

		if (m_buf_end < 0)
		{
			m_buf_end = 0;
			if (errno == EINTR || errno == EBUSY || errno == EAGAIN)
				continue;
			if (errno == EOVERFLOW)
			{
				eWarning("OVERFLOW while recording");
				continue;
			}
			eDebug("eFilePushThread *read error* (%m) - not yet handled");
		}

			/* a read might be mis-aligned in case of a short read. */
		int d = m_buf_end % m_blocksize;
		if (d)
			m_buf_end -= d;

		if (m_buf_end == 0)
		{
				/* on EOF, try COMMITting once. */
			if (m_send_pvr_commit)
			{
				struct pollfd pfd;
				pfd.fd = m_fd_dest;
				pfd.events = POLLIN;
				switch (poll(&pfd, 1, 250)) // wait for 250ms
				{
					case 0:
						eDebug("wait for driver eof timeout");
// vvv Fix to ensure that event evtEOF is called at end of playbackl part 2/3
						if(already_empty)
						{
							break;
						}
						else
						{
							already_empty=true;
							continue;
						}
// ^^^ Fix to ensure that event evtEOF is called at end of playbackl	part 2/3
					case 1:
						eDebug("wait for driver eof ok");
						break;
					default:
						eDebug("wait for driver eof aborted by signal");
						continue;
				}
			}
			
				/* in stream_mode, we are sending EOF events 
				   over and over until somebody responds.
				   
				   in stream_mode, think of evtEOF as "buffer underrun occured". */
			sendEvent(evtEOF);

			if (m_stream_mode)
			{
				eDebug("reached EOF, but we are in stream mode. delaying 1 second.");
				sleep(1);
				continue;
			}
			break;
		} else
		{
// vvv Fix to ensure that event evtEOF is called at end of playbackl part 3/3
			already_empty=false;
// ^^^ Fix to ensure that event evtEOF is called at end of playbackl part 3/3
			m_current_position += m_buf_end;
			bytes_read += m_buf_end;
			if (m_sg)
				current_span_remaining -= m_buf_end;
		}
//		printf("FILEPUSH: read %d bytes\n", m_buf_end);
	}
	// Do NOT call "fdatasync(m_fd_dest);" here because on some systems it doesn't return
	// and freezes the whole box.
	// Calling this function here is not that important, anyway, because the initiator closes
	// m_fd_dest immediatedly when the filepush thread has been stopped.
	// Original code has been:	
	// fdatasync(m_fd_dest);

#if defined(__sh__) // closes video device for the reverse playback workaround
	close(fd_video);
#endif

	eDebug("FILEPUSH THREAD STOP");
}

#if defined(__sh__) // here we prioritise and split the files
void eFilePushThread::start(int fd, int fd_dest, const char *filename)
{
	struct statfs sbuf;
	eRawFile *f = new eRawFile();
	ePtr<iTsSource> source = f;
	f->setfd(fd);
	start(source, fd_dest);
	m_record_split_size = 0;
	m_record_split_type = 0;
	m_flags = O_WRONLY|O_CREAT|O_LARGEFILE;

//FIXME: (schischu) This should be changed, 
//such values should come for e2 and not from an external source
	char record_split_size[3] = "";
	getshmentry(shm, "record_split_size=", record_split_size, 3);
	m_record_split_size = atoi(record_split_size);
	m_record_split_size = m_record_split_size * 1000 * 1000 * 1000;
	
	char record_split_type[2] = "";
	getshmentry(shm, "record_split_type=", record_split_type, 2);
	m_record_split_type = atoi(record_split_type);

	m_filename[0] = NULL;
	strcpy(m_filename, filename);

	if (statfs(m_filename, &sbuf) < 0)
	{
		eDebug("split record file - can't get fs type assuming none NFS!");
	} else
	{
		if (sbuf.f_type == EXT3_SUPER_MAGIC)
			eDebug("split record file - Ext2/3/4 Filesystem\n");
		else
		if (sbuf.f_type == NFS_SUPER_MAGIC)
		{
			eDebug("split record file - NFS Filesystem; add O_DIRECT to flags\n");
			m_flags |= O_DIRECT;
		}
		else
		if (sbuf.f_type == USBDEVICE_SUPER_MAGIC)
			eDebug("split record file - USB Device\n");
		else
		if (sbuf.f_type == SMB_SUPER_MAGIC)
			eDebug("split record file - SMBs Device\n");
		else
		if (sbuf.f_type == MSDOS_SUPER_MAGIC)
			eDebug("split record file - MSDOS Device\n");
	}

	resume();
}
#endif



void eFilePushThread::start(int fd, int fd_dest)
{
	eRawFile *f = new eRawFile();
	ePtr<iTsSource> source = f;
	f->setfd(fd);
	start(source, fd_dest);
}

int eFilePushThread::start(const char *file, int fd_dest)
{
	eRawFile *f = new eRawFile();
	ePtr<iTsSource> source = f;
	if (f->open(file) < 0)
		return -1;
	start(source, fd_dest);
	return 0;
}

void eFilePushThread::start(ePtr<iTsSource> &source, int fd_dest)
{
	m_source = source;
	m_fd_dest = fd_dest;
	m_current_position = 0;
	resume();
}

void eFilePushThread::stop()
{
		/* if we aren't running, don't bother stopping. */
	if (!sync())
		return;

	m_stop = 1;

	eDebug("stopping thread."); /* just do it ONCE. it won't help to do this more than once. */
	sendSignal(SIGUSR1);
	kill(0);
}

void eFilePushThread::pause()
{
	stop();
}

void eFilePushThread::resume()
{
	m_stop = 0;
	run();
}

void eFilePushThread::flush()
{
	m_buf_start = m_buf_end = m_filter_end = 0;
}

void eFilePushThread::enablePVRCommit(int s)
{
	m_send_pvr_commit = s;
}

void eFilePushThread::setStreamMode(int s)
{
	m_stream_mode = s;
}

void eFilePushThread::setScatterGather(iFilePushScatterGather *sg)
{
	m_sg = sg;
}

void eFilePushThread::sendEvent(int evt)
{
	m_messagepump.send(evt);
}

void eFilePushThread::recvEvent(const int &evt)
{
	m_event(evt);
}

int eFilePushThread::filterRecordData(const unsigned char *data, int len, size_t &current_span_remaining)
{
	return len;
}

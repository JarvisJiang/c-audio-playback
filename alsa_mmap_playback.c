#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
// unistd.h is typically POSIX, might need alternatives on non-POSIX systems
// For MinGW/Windows, you might need specific Windows headers for sleep functions
// or use a cross-platform library. For this example, we assume POSIX or similar.
#ifdef _WIN32
#include <windows.h>
#define sleep(seconds) Sleep((seconds) * 1000)
#define usleep(microseconds) Sleep((microseconds) / 1000) // Approximated
#else
#include <unistd.h> // For usleep/sleep
#endif


// --- Assumed variables (already initialized) ---
// snd_pcm_t *handle; // PCM device handle
// snd_pcm_uframes_t period_size; // Size of one period in frames
// unsigned int channels; // Number of audio channels
// snd_pcm_format_t format; // Sample format
// unsigned int rate; // Sample rate (Hz)
// int interleaved; // Whether the MMAP buffer is interleaved (usually 1 for MMAP)
// ---------------------------------------------

// Function to fill the buffer (replace with your actual audio data source)
void fill_buffer(const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
                 snd_pcm_uframes_t frames, snd_pcm_format_t format, unsigned int channels, int interleaved) {
    // Example: Fill with silence or generate a sine wave
    // This is where you would copy your audio data into the mapped buffer areas.
    // The structure of 'areas' depends on whether the format is interleaved or not.
    // For interleaved: areas[0].addr points to the buffer, areas[0].step is bits per sample.
    // For non-interleaved: areas[channel].addr points to the buffer for that channel.

    fprintf(stdout, "Filling buffer: offset=%lu, frames=%lu\n", offset, frames);

    // Placeholder: Zero out the buffer (silence)
    unsigned int chn;
    int bits_per_sample = snd_pcm_format_width(format);
    int bytes_per_frame = (bits_per_sample / 8) * channels;
    int bytes_per_sample = bits_per_sample / 8;

    if (interleaved) {
        // Calculate pointer based on area->addr + (offset * area->step / 8)
        // area->step is in bits, so divide by 8 for byte offset
        unsigned char *buffer_start = ((unsigned char *)areas[0].addr) + (offset * areas[0].step / 8);
        memset(buffer_start, 0, frames * bytes_per_frame);
    } else {
        for (chn = 0; chn < channels; ++chn) {
             // Calculate pointer based on area->addr + (offset * area->step / 8)
            unsigned char *channel_buffer = ((unsigned char *)areas[chn].addr) + (offset * areas[chn].step / 8);
             memset(channel_buffer, 0, frames * bytes_per_sample);
        }
    }
    // --- End Placeholder ---
}


int run_alsa_mmap_loop(snd_pcm_t *handle, snd_pcm_uframes_t period_size, snd_pcm_format_t format, unsigned int channels, int interleaved) {
    int err;
    snd_pcm_uframes_t offset, frames, avail;
    const snd_pcm_channel_area_t *areas;
    snd_pcm_status_t *status;

    snd_pcm_status_alloca(&status);

    while (1) { // Main playback loop
        // Wait for the PCM device to be ready for writing
        // Timeout set to 1 second (1000 ms)
        err = snd_pcm_wait(handle, 1000);
        if (err < 0) {
            fprintf(stderr, "PCM wait error: %s\n", snd_strerror(err));
            // Handle buffer underrun (XRUN) if err == -EPIPE
            if (err == -EPIPE) {
                fprintf(stderr, "Buffer underrun occurred (wait)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    return err;
                }
                continue; // Try waiting again
            } else {
                return err; // Other wait error
            }
        } else if (err == 0) {
             fprintf(stderr, "PCM wait timed out after 1 second.\n");
             // Timeout might indicate a problem, check status
             if (snd_pcm_status(handle, status) == 0) {
                 snd_pcm_state_t state = snd_pcm_status_get_state(status);
                 fprintf(stderr, "PCM state after timeout: %s\n", snd_pcm_state_name(state));
                 if (state == SND_PCM_STATE_XRUN) {
                     fprintf(stderr, "XRUN detected after wait timeout!\n");
                     err = snd_pcm_prepare(handle);
                     if (err < 0) return err;
                 } else if (state == SND_PCM_STATE_SUSPENDED) {
                     fprintf(stderr, "PCM suspended after wait timeout!\n");
                     while ((err = snd_pcm_resume(handle)) == -EAGAIN) sleep(1);
                     if (err < 0) {
                         err = snd_pcm_prepare(handle);
                         if (err < 0) return err;
                     }
                 }
             }
             continue; // Timeout, just try again
        }


        // Find out how much space is available
        avail = snd_pcm_avail_update(handle);
        if (avail < 0) {
            fprintf(stderr, "PCM avail update error: %s\n", snd_strerror((int)avail));
             if (avail == -EPIPE) {
                fprintf(stderr, "Buffer underrun occurred (avail_update)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    return err;
                }
                continue; // Try again
            } else {
                // Other error, potentially fatal for this stream
                return (int)avail;
            }
        }

        // We need at least one period's worth of frames for MMAP
        if (avail < period_size) {
             // Check PCM state if avail is low but wait didn't report error
             // This helps catch state changes that happened between wait and avail_update
             if (snd_pcm_status(handle, status) == 0) {
                 snd_pcm_state_t state = snd_pcm_status_get_state(status);
                 if (state == SND_PCM_STATE_XRUN) {
                     fprintf(stderr, "XRUN detected by status (avail < period_size)!\n");
                     err = snd_pcm_prepare(handle);
                     if (err < 0) {
                         fprintf(stderr, "Failed to recover from XRUN: %s\n", snd_strerror(err));
                         return err;
                     }
                     continue; // Retry loop
                 } else if (state == SND_PCM_STATE_SUSPENDED) {
                      fprintf(stderr, "PCM suspended (avail < period_size)! Attempting resume...\n");
                      // For suspend, often need prepare after resume
                      while ((err = snd_pcm_resume(handle)) == -EAGAIN)
                           sleep(1); // Wait until suspend flag is released by kernel/system
                      if (err < 0) {
                           // Resume failed, try prepare
                           err = snd_pcm_prepare(handle);
                           if (err < 0) {
                                fprintf(stderr, "Failed to recover from suspend: %s\n", snd_strerror(err));
                                return err;
                           }
                      }
                      continue; // Retry loop
                 } else if (state == SND_PCM_STATE_SETUP || state == SND_PCM_STATE_PREPARED) {
                     // Device might need starting if it stopped unexpectedly
                     // Or maybe just needs more data, wait loop handles this
                     //fprintf(stdout, "PCM state is %s, waiting for more space.\n", snd_pcm_state_name(state));
                 } else if (state == SND_PCM_STATE_RUNNING) {
                     // Normal state, just not enough space yet
                 } else {
                     fprintf(stderr, "Unexpected PCM state %s when avail < period_size.\n", snd_pcm_state_name(state));
                 }
             }
            // Not enough space yet, wait a bit and loop again
            // This can happen if the period size is large or system is busy
            //fprintf(stdout, "Waiting for buffer space (avail=%lu, needed=%lu)\n", avail, period_size);
            usleep(10000); // Wait 10ms before checking again
            continue;
        }

        // We have enough space, let's try to write one period
        frames = period_size;

        // Request access to the MMAP buffer area
        err = snd_pcm_mmap_begin(handle, &areas, &offset, &frames);
        if (err < 0) {
            fprintf(stderr, "MMAP begin error: %s\n", snd_strerror(err));
             if (err == -EPIPE) {
                fprintf(stderr, "Buffer underrun occurred (mmap_begin)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    return err;
                }
                continue; // Try again
            } else if (err == -ESTRPIPE) { // Stream is suspended
                 fprintf(stderr, "Stream is suspended (mmap_begin)! Attempting resume...\n");
                 while ((err = snd_pcm_resume(handle)) == -EAGAIN)
                     sleep(1); // Wait until suspend flag is released
                 if (err < 0) {
                     // Resume failed, try prepare
                     err = snd_pcm_prepare(handle);
                     if (err < 0) {
                         fprintf(stderr, "Failed to recover from suspend: %s\n", snd_strerror(err));
                         return err;
                     }
                 }
                 continue; // Retry loop
            } else {
                // Other error, potentially fatal
                return err;
            }
        }

        // Check if we got the number of frames we asked for
        // This shouldn't happen often if avail >= period_size, but good to check
        if (frames < period_size) {
            fprintf(stderr, "Warning: mmap_begin returned less frames (%lu) than requested (%lu)\n", frames, period_size);
            // This might indicate a configuration issue or an XRUN is imminent
            // We could try to handle this, but for simplicity, we'll proceed if frames > 0
            if (frames == 0) {
                 fprintf(stderr, "Error: mmap_begin returned 0 frames. Preparing stream.\n");
                 // Attempt to commit 0 frames to potentially clear state, then prepare
                 snd_pcm_mmap_commit(handle, offset, 0); // Commit might fail, ignore error here
                 err = snd_pcm_prepare(handle);
                 if (err < 0) {
                     fprintf(stderr, "Failed to prepare after 0 frames from mmap_begin: %s\n", snd_strerror(err));
                     return err;
                 }
                 continue; // Retry loop
            }
            // If frames > 0 but < period_size, we fill what we got
        }

        // --- Fill the buffer with your audio data ---
        // Pass the actual number of frames obtained ('frames')
        fill_buffer(areas, offset, frames, format, channels, interleaved);
        // --------------------------------------------

        // Commit the frames (tell ALSA we've written them)
        snd_pcm_sframes_t committed = snd_pcm_mmap_commit(handle, offset, frames);
        if (committed < 0 || (snd_pcm_uframes_t)committed != frames) {
            fprintf(stderr, "MMAP commit error or mismatch: %s (committed=%ld, expected=%lu)\n",
                    snd_strerror((int)committed), committed, frames);
            if (committed == -EPIPE) {
                 fprintf(stderr, "Buffer underrun occurred (mmap_commit)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    return err;
                }
                // Don't continue immediately, let the loop re-evaluate state
            } else if (committed == -ESTRPIPE) { // Stream is suspended
                 fprintf(stderr, "Stream is suspended (mmap_commit)! Attempting resume...\n");
                 while ((err = snd_pcm_resume(handle)) == -EAGAIN)
                     sleep(1); // Wait until suspend flag is released
                 if (err < 0) {
                     // Resume failed, try prepare
                     err = snd_pcm_prepare(handle);
                     if (err < 0) {
                         fprintf(stderr, "Failed to recover from suspend: %s\n", snd_strerror(err));
                         return err;
                     }
                 }
            } else {
                 // Other commit error, might need prepare
                 fprintf(stderr, "Unknown commit error, preparing stream.\n");
                 err = snd_pcm_prepare(handle);
                 if (err < 0) {
                     fprintf(stderr, "Failed to prepare after commit error: %s\n", snd_strerror(err));
                     return err;
                 }
            }
             continue; // Re-check state in the next loop iteration
        }

        // Optional: Add a small sleep if CPU usage is too high,
        // but snd_pcm_wait should handle blocking efficiently.
        // usleep(1000);
    }

    // snd_pcm_status_free(status); // Not needed with alloca
    return 0; // Should not be reached in an infinite loop
}

/*
// --- Example Usage Placeholder ---
int main() {
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size = 1024; // Example period size
    unsigned int rate = 44100;
    unsigned int channels = 2;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    int err;
    int interleaved = 1; // Use interleaved MMAP

    const char *device = "default"; // Or "plughw:0,0" etc.

    // 1. Open PCM device
    // Use SND_PCM_NONBLOCK for snd_pcm_wait timeout to work correctly
    // and to handle suspend/resume properly.
    err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "Cannot open audio device %s (%s)\n", device, snd_strerror(err));
        return 1;
    }
    printf("Audio device opened: %s\n", device);

    // 2. Allocate HW params object
    snd_pcm_hw_params_alloca(&hw_params);

    // 3. Fill it with default values
    err = snd_pcm_hw_params_any(handle, hw_params);
    if (err < 0) { fprintf(stderr, "Error getting default hw params: %s\n", snd_strerror(err)); snd_pcm_close(handle); return 1; }


    // 4. Set HW parameters
    // *** IMPORTANT: Set Access Mode to MMAP Interleaved or Non-Interleaved ***
    snd_pcm_access_t access = interleaved ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
    err = snd_pcm_hw_params_set_access(handle, hw_params, access);
    if (err < 0) { fprintf(stderr, "Error setting access %s: %s\n", interleaved?"interleaved":"non-interleaved", snd_strerror(err)); snd_pcm_close(handle); return 1; }
    printf("Access type set to: %s\n", snd_pcm_access_name(access));

    err = snd_pcm_hw_params_set_format(handle, hw_params, format);
     if (err < 0) { fprintf(stderr, "Error setting format %s: %s\n", snd_pcm_format_name(format), snd_strerror(err)); snd_pcm_close(handle); return 1; }
     printf("Format set to: %s\n", snd_pcm_format_name(format));


    err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0);
     if (err < 0) { fprintf(stderr, "Error setting rate near %u Hz: %s\n", rate, snd_strerror(err)); snd_pcm_close(handle); return 1; }
     printf("Rate set near: %u Hz\n", rate);


    err = snd_pcm_hw_params_set_channels(handle, hw_params, channels);
     if (err < 0) { fprintf(stderr, "Error setting channels to %u: %s\n", channels, snd_strerror(err)); snd_pcm_close(handle); return 1; }
     printf("Channels set to: %u\n", channels);


    // Set period size
    int dir = 0; // Use 0 for default direction preference
    err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, &dir);
    if (err < 0) { fprintf(stderr, "Error setting period size near %lu: %s\n", period_size, snd_strerror(err)); snd_pcm_close(handle); return 1; }
    err = snd_pcm_hw_params_get_period_size(hw_params, &period_size, &dir); // Get the actual size set
    if (err < 0) { fprintf(stderr, "Error getting period size: %s\n", snd_strerror(err)); snd_pcm_close(handle); return 1; }
    printf("Period size set to: %lu frames\n", period_size);


    // Set buffer size (usually multiple periods, e.g., 2 to 4)
    snd_pcm_uframes_t target_buffer_size = period_size * 4;
    err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &target_buffer_size);
     if (err < 0) { fprintf(stderr, "Error setting buffer size near %lu: %s\n", target_buffer_size, snd_strerror(err)); snd_pcm_close(handle); return 1; }

    // Get actual buffer size
    err = snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);
     if (err < 0) { fprintf(stderr, "Error getting buffer size: %s\n", snd_strerror(err)); snd_pcm_close(handle); return 1; }
     printf("Buffer size set to: %lu frames (%lu periods)\n", buffer_size, buffer_size / period_size);


    // 5. Write HW parameters to the driver
    err = snd_pcm_hw_params(handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "Unable to set hw params for playback: %s\n", snd_strerror(err));
        snd_pcm_close(handle);
        return 1;
    }
    printf("Hardware parameters set successfully.\n");

    // 6. Set SW parameters (optional but recommended for MMAP)
    snd_pcm_sw_params_alloca(&sw_params);
    err = snd_pcm_sw_params_current(handle, sw_params);
    if (err < 0) { fprintf(stderr, "Error getting current sw params: %s\n", snd_strerror(err)); snd_pcm_close(handle); return 1; }


    // Start threshold: Start playing when buffer is at least this full (e.g., one period)
    // For MMAP, often set close to buffer_size - period_size or just period_size.
    // Setting it to period_size ensures playback starts quickly after first commit.
    err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, period_size);
     if (err < 0) { fprintf(stderr, "Error setting start threshold: %s\n", snd_strerror(err)); }
    else { snd_pcm_sw_params_get_start_threshold(sw_params, &target_buffer_size); printf("Start threshold set to: %lu frames\n", target_buffer_size); }


    // Available minimum: Wake us up when at least period_size frames are available for writing
    // This is crucial for snd_pcm_wait() to work efficiently with MMAP.
    err = snd_pcm_sw_params_set_avail_min(handle, sw_params, period_size);
     if (err < 0) { fprintf(stderr, "Error setting avail min: %s\n", snd_strerror(err)); }
     else { snd_pcm_sw_params_get_avail_min(sw_params, &target_buffer_size); printf("Avail min set to: %lu frames\n", target_buffer_size); }


    // Set period event generation (optional, can be useful for precise timing via poll/select)
    // err = snd_pcm_sw_params_set_period_event(handle, sw_params, 1);
    // if (err < 0) { fprintf(stderr, "Error setting period event: %s\n", snd_strerror(err)); }


    // Write the software parameters.
    err = snd_pcm_sw_params(handle, sw_params);
     if (err < 0) { fprintf(stderr, "Error setting sw params: %s\n", snd_strerror(err)); }
     else { printf("Software parameters set successfully.\n"); }


    // 7. Run the MMAP loop
    printf("Starting ALSA MMAP playback loop...\n");
    err = run_alsa_mmap_loop(handle, period_size, format, channels, interleaved);
    if (err < 0) {
        fprintf(stderr, "Playback loop failed: %s\n", snd_strerror(err));
    }

    // 8. Clean up
    printf("Attempting to close audio stream...\n");
    // snd_pcm_drain(handle); // Drain might block indefinitely in non-blocking mode if errors occurred
    err = snd_pcm_drop(handle); // Drop is safer for non-blocking cleanup after errors
    if (err < 0) { fprintf(stderr, "Error dropping PCM stream: %s\n", snd_strerror(err)); }
    err = snd_pcm_close(handle);
    if (err < 0) { fprintf(stderr, "Error closing PCM handle: %s\n", snd_strerror(err)); }
    else { printf("Audio device closed.\n"); }
    printf("Playback finished.\n");

    return 0;
}
*/
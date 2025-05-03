#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memset
#include <unistd.h> // For usleep/sleep - Linux specific
#include <math.h>   // For sin()
#include <pthread.h> // For threading
#include <signal.h> // For signal handling (optional shutdown)

// --- Compilation Instructions (Linux) ---
// You need the ALSA development library installed (e.g., libasound2-dev on Debian/Ubuntu)
// Compile using GCC:
// gcc alsa_mmap_playback.c -o alsa_mmap_playback -lasound -lm -lpthread
//
// Run:
// ./alsa_mmap_playback
// -----------------------------------------


// --- Assumed variables (already initialized within run_alsa_mmap_loop) ---
// snd_pcm_t *handle; // PCM device handle
// snd_pcm_uframes_t period_size; // Size of one period in frames
// unsigned int channels; // Number of audio channels
// snd_pcm_format_t format; // Sample format
// unsigned int rate; // Sample rate (Hz)
// int interleaved; // Whether the MMAP buffer is interleaved (usually 1 for MMAP)
// ---------------------------------------------

// Global state for audio generation
static double g_phase1 = 0.0;
static double g_phase2 = 0.0;
static double g_phase3 = 0.0;
static unsigned long long g_sample_count = 0;
static double g_freq2 = 220.0; // Start frequency for second oscillator
static double g_lfo_phase = 0.0;

#ifndef PI
#define PI 3.14159265358979323846
#endif

// --- Ring Buffer Implementation ---
typedef struct {
    short *buffer;          // Data buffer
    size_t size;            // Size of buffer in samples (total capacity)
    size_t write_pos;       // Index to write next sample
    size_t read_pos;        // Index to read next sample
    size_t count;           // Number of samples currently in buffer
} ring_buffer_t;

// --- Shared Data Structure for Threads ---
typedef struct {
    snd_pcm_t *playback_handle;
    snd_pcm_t *capture_handle;
    snd_pcm_uframes_t period_size;
    snd_pcm_format_t format;
    unsigned int channels;
    int playback_interleaved;

    ring_buffer_t shared_buffer; // Ring buffer for captured audio
    pthread_mutex_t buffer_mutex; // Mutex to protect the buffer
    pthread_cond_t cond_not_full; // Condition: buffer is not full (capture waits)
    pthread_cond_t cond_not_empty; // Condition: buffer is not empty (playback waits)

    volatile sig_atomic_t running; // Flag to signal threads to stop
} audio_thread_data_t;


// Simple low-pass filter state for noise
static double noise_lpf_state = 0.0;

// Generates a single audio sample (S16_LE format assumed) trying for a "powerful" sound
// Updates global phase/state variables
short generate_audio_sample() {
    // Assuming 44100 Hz rate
    double rate = 44100.0;
    short max_amplitude = 32760;

    // --- Oscillators ---
    double freq1 = 55.0; // Lower drone (A1)

    // LFO for modulation
    double lfo_freq = 1.0; // Slightly faster LFO
    double lfo_amp_mod_depth = 0.6; // Deeper tremolo
    double lfo_freq_mod_depth = 30.0; // Less extreme vibrato

    // Frequency sweep for osc2 (A3 to A4 and back over 8 seconds)
    double sweep_period_samples = 8.0 * rate;
    double time_in_sweep = fmod((double)g_sample_count, sweep_period_samples);
    double sweep_progress = time_in_sweep / sweep_period_samples;
    if (sweep_progress < 0.5) {
         g_freq2 = 220.0 + (440.0 - 220.0) * (sweep_progress * 2.0); // Sweep up A3->A4
    } else {
         g_freq2 = 440.0 - (440.0 - 220.0) * ((sweep_progress - 0.5) * 2.0); // Sweep down A4->A3
    }
    double freq3 = g_freq2 * 1.5; // A fifth above osc2 (approx E4/E5)

    // Calculate LFO value (sine wave)
    double lfo_val = sin(g_lfo_phase);
    g_lfo_phase += 2.0 * PI * lfo_freq / rate;
    if (g_lfo_phase >= 2.0 * PI) g_lfo_phase -= 2.0 * PI;

    // Modulate frequencies and amplitudes
    double current_freq2 = g_freq2 + lfo_val * lfo_freq_mod_depth; // Vibrato on osc2
    double current_freq3 = freq3; // Keep base freq3 stable for now
    double amp3_mod = 1.0 - (lfo_amp_mod_depth * (1.0 + lfo_val) / 2.0); // Tremolo on osc3

    // --- Generate Waveforms ---
    // Osc1: Simple Sine Drone
    double sample1 = sin(g_phase1);

    // Osc2: Sawtooth approximation (fundamental + first 3 harmonics)
    double sample2 = sin(g_phase2);
    sample2 += 0.5 * sin(2.0 * g_phase2);
    sample2 += 0.33 * sin(3.0 * g_phase2);
    sample2 += 0.25 * sin(4.0 * g_phase2);
    sample2 *= 0.6; // Normalize roughly

    // Osc3: Square wave approximation (fundamental + 2 odd harmonics)
    double sample3 = sin(g_phase3);
    sample3 += 0.33 * sin(3.0 * g_phase3);
    sample3 += 0.2 * sin(5.0 * g_phase3);
    sample3 *= 0.7; // Normalize roughly
    sample3 *= amp3_mod; // Apply tremolo

    // Noise Component: Simple white noise -> low-pass filter
    double noise = ((double)rand() / RAND_MAX) * 2.0 - 1.0; // White noise -1 to 1
    double lpf_cutoff = 0.1; // Adjust cutoff frequency (lower = more muffled)
    noise_lpf_state += lpf_cutoff * (noise - noise_lpf_state);
    double filtered_noise = noise_lpf_state;


    // --- Combine Components ---
    // Adjust amplitudes for a more "powerful" mix (more bass, more noise)
    double combined_sample = (sample1 * 0.4) + (sample2 * 0.3) + (sample3 * 0.2) + (filtered_noise * 0.15);

    // Simple clipping (alternative to scaling down max_amplitude)
    if (combined_sample > 1.0) combined_sample = 1.0;
    if (combined_sample < -1.0) combined_sample = -1.0;

    // Convert to S16_LE format
    short pcm_sample = (short)(combined_sample * max_amplitude);

    // --- Update Phases ---
    g_phase1 += 2.0 * PI * freq1 / rate;
    if (g_phase1 >= 2.0 * PI) g_phase1 -= 2.0 * PI;

    // Use the *modulated* frequency for phase update
    g_phase2 += 2.0 * PI * current_freq2 / rate;
    if (g_phase2 >= 2.0 * PI) g_phase2 -= 2.0 * PI;

    // Use the *unmodulated* frequency for phase update (tremolo affects amplitude only)
    g_phase3 += 2.0 * PI * current_freq3 / rate;
     if (g_phase3 >= 2.0 * PI) g_phase3 -= 2.0 * PI;

    g_sample_count++; // Increment global sample counter

    return pcm_sample;
}

// Function to fill the playback buffer by mixing generated audio and captured audio
void mix_and_fill_buffer(const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
                         snd_pcm_uframes_t frames, snd_pcm_format_t format, unsigned int channels, int interleaved,
                         audio_thread_data_t *data) {

    // Assuming SND_PCM_FORMAT_S16_LE
    int bits_per_sample = 16;
    int bytes_per_sample = bits_per_sample / 8;
    short max_amplitude = 32760;
    ring_buffer_t *rb = &data->shared_buffer;
    short captured_sample = 0;

    for (snd_pcm_uframes_t i = 0; i < frames; ++i) {
        short generated_sample = generate_audio_sample();
        captured_sample = 0; // Default to silence if capture buffer is empty

        // --- Read from shared ring buffer ---
        pthread_mutex_lock(&data->buffer_mutex);
        // Wait until there's data or we are shutting down
        while (rb->count == 0 && data->running) {
            // Optional: Timeout for pthread_cond_timedwait if needed
            pthread_cond_wait(&data->cond_not_empty, &data->buffer_mutex);
        }

        if (data->running && rb->count > 0) {
            captured_sample = rb->buffer[rb->read_pos];
            rb->read_pos = (rb->read_pos + 1) % rb->size;
            rb->count--;
            // Signal capture thread that there's space now
            pthread_cond_signal(&data->cond_not_full);
        }
        pthread_mutex_unlock(&data->buffer_mutex);
        // --- End Read ---

        if (!data->running) break; // Exit loop if shutting down

        short mixed_sample;
        // Simple mix - adjust volumes (e.g., 50% generated, 50% captured)
        long temp_mix = (long)generated_sample * 0.5 + (long)captured_sample * 0.5;

        // Clipping
        if (temp_mix > max_amplitude) mixed_sample = max_amplitude;
        else if (temp_mix < -max_amplitude) mixed_sample = -max_amplitude;
        else mixed_sample = (short)temp_mix;


        // Write mixed sample to playback buffer
        for (unsigned int chn = 0; chn < channels; ++chn) {
            unsigned char *ptr;
            if (interleaved) {
                ptr = ((unsigned char *)areas[0].addr) + ((offset + i) * areas[0].step / 8) + (chn * bytes_per_sample);
            } else {
                ptr = ((unsigned char *)areas[chn].addr) + ((offset + i) * areas[chn].step / 8);
            }
            ptr[0] = (unsigned char)(mixed_sample & 0xFF);
            ptr[1] = (unsigned char)((mixed_sample >> 8) & 0xFF);
        }
    }
}


// Playback loop function for the playback thread
void *playback_thread_func(void *arg) {
    audio_thread_data_t *data = (audio_thread_data_t *)arg;
    snd_pcm_t *handle = data->playback_handle;
    snd_pcm_uframes_t period_size = data->period_size;
    snd_pcm_format_t format = data->format;
    unsigned int channels = data->channels;
    int interleaved = data->playback_interleaved;

    int err;
    snd_pcm_uframes_t offset, frames, avail;
    const snd_pcm_channel_area_t *areas;
    snd_pcm_status_t *status;

    printf("Playback thread started.\n");
    snd_pcm_status_alloca(&status);

    while (data->running) { // Main playback loop controlled by flag
        // Wait for the PCM device to be ready for writing
        // Timeout set to 100ms for responsiveness to shutdown signal
        err = snd_pcm_wait(handle, 100);
        if (err < 0) {
            fprintf(stderr, "PCM wait error: %s\n", snd_strerror(err));
            // Handle buffer underrun (XRUN) if err == -EPIPE
            if (err == -EPIPE) {
                fprintf(stderr, "Buffer underrun occurred (wait)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    data->running = 0; // Signal exit
                    return NULL;
                }
                continue; // Try waiting again
            } else {
                 data->running = 0; // Signal exit
                 return NULL; // Other wait error
            }
        } else if (err == 0) {
             //fprintf(stderr, "PCM wait timed out after 100ms.\n");
             // Timeout might indicate a problem, check status
             if (snd_pcm_status(handle, status) == 0) {
                 snd_pcm_state_t state = snd_pcm_status_get_state(status);
                 //fprintf(stderr, "PCM state after timeout: %s\n", snd_pcm_state_name(state));
                 if (state == SND_PCM_STATE_XRUN) {
                     fprintf(stderr, "XRUN detected after wait timeout!\n");
                     err = snd_pcm_prepare(handle);
                     if (err < 0) { data->running = 0; return NULL; }
                 } else if (state == SND_PCM_STATE_SUSPENDED) {
                     fprintf(stderr, "PCM suspended after wait timeout!\n");
                     while ((err = snd_pcm_resume(handle)) == -EAGAIN) sleep(1);
                     if (err < 0) {
                         err = snd_pcm_prepare(handle);
                         if (err < 0) { data->running = 0; return NULL; }
                     }
                 }
             }
             // Timeout is expected if buffer is full or during shutdown
             if (!data->running) break; // Exit if shutting down
             continue;
        }


        // Find out how much space is available
        avail = snd_pcm_avail_update(handle);
        if (!data->running) break; // Exit if shutting down
        if (avail < 0) {
            fprintf(stderr, "PCM avail update error: %s\n", snd_strerror((int)avail));
             if (avail == -EPIPE) {
                fprintf(stderr, "Buffer underrun occurred (avail_update)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    data->running = 0; return NULL;
                }
                continue; // Try again
            } else {
                // Other error, potentially fatal for this stream
                 data->running = 0; return NULL;
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
                         data->running = 0; return NULL;
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
                                data->running = 0; return NULL;
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
            usleep(5000); // Wait 5ms before checking again
            continue;
        }

        // We have enough space, let's try to write one period (or less if avail < period_size)
        frames = (avail >= period_size) ? period_size : avail;
        if (frames == 0) {
            usleep(5000);
            continue;
        }


        // Request access to the MMAP buffer area
        err = snd_pcm_mmap_begin(handle, &areas, &offset, &frames);
        if (!data->running) break; // Exit if shutting down
        if (err < 0) {
            fprintf(stderr, "MMAP begin error: %s\n", snd_strerror(err));
             if (err == -EPIPE) {
                fprintf(stderr, "Buffer underrun occurred (mmap_begin)! Trying to recover...\n");
                err = snd_pcm_prepare(handle); // Try to recover
                if (err < 0) {
                    fprintf(stderr, "Failed to recover from underrun: %s\n", snd_strerror(err));
                    data->running = 0; return NULL;
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
                         data->running = 0; return NULL;
                     }
                 }
                 continue; // Retry loop
            } else {
                // Other error, potentially fatal
                 data->running = 0; return NULL;
            }
        }

        // Check if we got the number of frames we asked for
        // This shouldn't happen often if avail >= period_size, but good to check
        if (frames < period_size && avail >= period_size) { // Only warn if we expected full period
            fprintf(stderr, "Warning: mmap_begin returned less frames (%lu) than requested (%lu) despite avail=%lu\n", frames, period_size, avail);
        }
        if (frames == 0) {
             fprintf(stderr, "Error: mmap_begin returned 0 frames. Preparing stream.\n");
             // Attempt to commit 0 frames to potentially clear state, then prepare
             snd_pcm_mmap_commit(handle, offset, 0); // Commit might fail, ignore error here
             err = snd_pcm_prepare(handle);
             if (err < 0) {
                 fprintf(stderr, "Failed to prepare after 0 frames from mmap_begin: %s\n", snd_strerror(err));
                 data->running = 0; return NULL;
             }
             continue; // Retry loop
        }

        // --- Fill the buffer with mixed audio data ---
        mix_and_fill_buffer(areas, offset, frames, format, channels, interleaved, data);
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
                    data->running = 0; return NULL;
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
                         data->running = 0; return NULL;
                     }
                 }
            } else {
                 // Other commit error, might need prepare
                 fprintf(stderr, "Unknown commit error, preparing stream.\n");
                 err = snd_pcm_prepare(handle);
                 if (err < 0) {
                     fprintf(stderr, "Failed to prepare after commit error: %s\n", snd_strerror(err));
                     data->running = 0; return NULL;
                 }
            }
             if (!data->running) break; // Exit if shutting down
             continue; // Re-check state in the next loop iteration
        }
    }

    printf("Playback thread finished.\n");
    return NULL;
}


// Capture loop function for the capture thread (using readi)
void *capture_thread_func(void *arg) {
    audio_thread_data_t *data = (audio_thread_data_t *)arg;
    snd_pcm_t *handle = data->capture_handle;
    snd_pcm_uframes_t period_size = data->period_size;
    unsigned int channels = data->channels;
    ring_buffer_t *rb = &data->shared_buffer;
    int err;
    snd_pcm_sframes_t frames_read;

    // Allocate local buffer for reading one period
    int bytes_per_frame = snd_pcm_format_width(data->format) / 8 * channels;
    short *local_buffer = malloc(period_size * bytes_per_frame);
    if (!local_buffer) {
        fprintf(stderr, "Capture thread: Failed to allocate local buffer\n");
        data->running = 0; // Signal other threads to stop
        return NULL;
    }

    printf("Capture thread started.\n");

    while (data->running) {
        frames_read = snd_pcm_readi(handle, local_buffer, period_size);

        if (!data->running) break; // Check flag after potentially blocking read

        if (frames_read < 0) {
            fprintf(stderr, "Capture error: %s\n", snd_strerror(frames_read));
            if (frames_read == -EPIPE) { // Overrun
                fprintf(stderr, "Capture overrun occurred! Preparing...\n");
                err = snd_pcm_prepare(handle);
                if (err < 0) { fprintf(stderr, "Capture prepare failed: %s\n", snd_strerror(err)); data->running = 0; break; }
                continue;
            } else if (frames_read == -ESTRPIPE) { // Suspended
                 fprintf(stderr, "Capture stream suspended! Attempting resume...\n");
                 while ((err = snd_pcm_resume(handle)) == -EAGAIN) sleep(1);
                 if (err < 0) {
                     err = snd_pcm_prepare(handle);
                     if (err < 0) { fprintf(stderr, "Capture recover failed: %s\n", snd_strerror(err)); data->running = 0; break; }
                 }
                 continue;
            } else { data->running = 0; break; } // Other fatal error
        } else if ((snd_pcm_uframes_t)frames_read != period_size) {
            fprintf(stderr, "Capture short read: %ld frames instead of %lu\n", frames_read, period_size);
        }

        if (frames_read > 0) {
            // --- Write captured data to shared ring buffer ---
            size_t samples_to_write = frames_read * channels; // Assuming interleaved S16_LE
            size_t samples_written = 0;

            pthread_mutex_lock(&data->buffer_mutex);
            while (samples_written < samples_to_write && data->running) {
                // Wait if buffer is full
                while (rb->count >= rb->size && data->running) {
                    pthread_cond_wait(&data->cond_not_full, &data->buffer_mutex);
                }
                if (!data->running) break; // Check again after wait

                // Write samples until buffer is full or all samples are written
                size_t available_space = rb->size - rb->count;
                size_t remaining_samples = samples_to_write - samples_written;
                size_t chunk_size = (available_space < remaining_samples) ? available_space : remaining_samples;

                // Copy in potentially two parts if wrap-around occurs
                size_t part1_size = (rb->write_pos + chunk_size > rb->size) ? (rb->size - rb->write_pos) : chunk_size;
                memcpy(rb->buffer + rb->write_pos, local_buffer + samples_written, part1_size * sizeof(short));

                size_t part2_size = chunk_size - part1_size;
                if (part2_size > 0) {
                    memcpy(rb->buffer, local_buffer + samples_written + part1_size, part2_size * sizeof(short));
                }

                rb->write_pos = (rb->write_pos + chunk_size) % rb->size;
                rb->count += chunk_size;
                samples_written += chunk_size;

                // Signal playback thread that data is available
                pthread_cond_signal(&data->cond_not_empty);
            }
            pthread_mutex_unlock(&data->buffer_mutex);
            // --- End Write ---
        }
    }

    free(local_buffer);
    printf("Capture thread finished.\n");
    return NULL;
}


// Function to list available sound cards
void list_sound_cards() {
    int card = -1;
    int err;
    char card_name[32];

    printf("Available ALSA Sound Cards:\n");
    printf("---------------------------\n");

    if ((err = snd_card_next(&card)) < 0) {
        fprintf(stderr, "Cannot get first card number: %s\n", snd_strerror(err));
        return;
    }
    if (card < 0) {
        fprintf(stderr, "No sound cards found.\n");
        return;
    }

    while (card >= 0) {
        snd_ctl_t *handle;
        snd_ctl_card_info_t *info;
        char ctl_name[32];

        sprintf(ctl_name, "hw:%d", card);
        if ((err = snd_ctl_open(&handle, ctl_name, 0)) < 0) {
            fprintf(stderr, "Cannot open control for card %d: %s\n", card, snd_strerror(err));
            goto next_card;
        }

        snd_ctl_card_info_alloca(&info);
        if ((err = snd_ctl_card_info(handle, info)) < 0) {
            fprintf(stderr, "Cannot get card info for card %d: %s\n", card, snd_strerror(err));
            snd_ctl_close(handle);
            goto next_card;
        }

        printf("Card %d: [%s] - %s\n", card, snd_ctl_card_info_get_id(info), snd_ctl_card_info_get_name(info));
        // You could also get snd_ctl_card_info_get_longname(info) for more details

        snd_ctl_close(handle);

    next_card:
        if ((err = snd_card_next(&card)) < 0) {
            fprintf(stderr, "Cannot get next card number: %s\n", snd_strerror(err));
            break;
        }
    }
     printf("---------------------------\n");
}


// Global pointer to thread data for signal handler
static audio_thread_data_t *g_thread_data_ptr = NULL;

// Signal handler for clean shutdown
void signal_handler(int sig) {
    printf("\nCaught signal %d, signaling threads to stop...\n", sig);
    if (g_thread_data_ptr) {
        g_thread_data_ptr->running = 0;
        // Wake up threads potentially waiting on condition variables
        pthread_cond_signal(&g_thread_data_ptr->cond_not_empty);
        pthread_cond_signal(&g_thread_data_ptr->cond_not_full);
    }
    // Don't exit here, let main join threads
}


// --- Main Function ---
int main() {
    // List sound cards first
    list_sound_cards();

    // --- Playback Setup ---
    snd_pcm_t *playback_handle;
    snd_pcm_hw_params_t *playback_hw_params;
    snd_pcm_sw_params_t *playback_sw_params;

    // --- Capture Setup ---
    snd_pcm_t *capture_handle;
    snd_pcm_hw_params_t *capture_hw_params;
    // snd_pcm_sw_params_t *capture_sw_params; // SW params less critical for basic readi capture

    // --- Common Parameters ---
    snd_pcm_uframes_t playback_buffer_size;
    snd_pcm_uframes_t capture_buffer_size; // May differ from playback
    snd_pcm_uframes_t period_size = 1024; // Keep period size consistent for simplicity
    unsigned int rate = 44100;
    unsigned int channels = 2;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    int err;
    int playback_interleaved = 1; // Use interleaved MMAP for playback

    const char *playback_device = "default"; // Or "plughw:0,0" etc.
    const char *capture_device = "default";  // Or specific capture device like "hw:0,0"

    // --- Threading Setup ---
    pthread_t playback_tid, capture_tid;
    audio_thread_data_t thread_data;
    g_thread_data_ptr = &thread_data; // Set global pointer for signal handler
    thread_data.running = 1; // Set running flag

    // --- Setup Playback Stream ---
    printf("--- Setting up Playback Stream (%s) ---\n", playback_device);
    err = snd_pcm_open(&playback_handle, playback_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) { fprintf(stderr, "Cannot open playback device %s (%s)\n", playback_device, snd_strerror(err)); return 1; }
    printf("Playback device opened.\n");
    snd_pcm_hw_params_alloca(&playback_hw_params);
    err = snd_pcm_hw_params_any(playback_handle, playback_hw_params);
    if (err < 0) { fprintf(stderr, "Playback: Error getting default hw params: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }

    // Set Playback HW parameters (MMAP Interleaved)
    snd_pcm_access_t playback_access = playback_interleaved ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
    err = snd_pcm_hw_params_set_access(playback_handle, playback_hw_params, playback_access);
    if (err < 0) { fprintf(stderr, "Playback: Error setting access %s: %s\n", snd_pcm_access_name(playback_access), snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    err = snd_pcm_hw_params_set_format(playback_handle, playback_hw_params, format);
    if (err < 0) { fprintf(stderr, "Playback: Error setting format %s: %s\n", snd_pcm_format_name(format), snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    err = snd_pcm_hw_params_set_rate_near(playback_handle, playback_hw_params, &rate, 0);
    if (err < 0) { fprintf(stderr, "Playback: Error setting rate near %u Hz: %s\n", rate, snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    err = snd_pcm_hw_params_set_channels(playback_handle, playback_hw_params, channels);
    if (err < 0) { fprintf(stderr, "Playback: Error setting channels to %u: %s\n", channels, snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    int dir = 0;
    err = snd_pcm_hw_params_set_period_size_near(playback_handle, playback_hw_params, &period_size, &dir);
    if (err < 0) { fprintf(stderr, "Playback: Error setting period size near %lu: %s\n", period_size, snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    err = snd_pcm_hw_params_get_period_size(playback_hw_params, &period_size, &dir);
    if (err < 0) { fprintf(stderr, "Playback: Error getting period size: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    snd_pcm_uframes_t target_buffer_size = period_size * 4;
    err = snd_pcm_hw_params_set_buffer_size_near(playback_handle, playback_hw_params, &target_buffer_size);
    if (err < 0) { fprintf(stderr, "Playback: Error setting buffer size near %lu: %s\n", target_buffer_size, snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    err = snd_pcm_hw_params_get_buffer_size(playback_hw_params, &playback_buffer_size);
    if (err < 0) { fprintf(stderr, "Playback: Error getting buffer size: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    err = snd_pcm_hw_params(playback_handle, playback_hw_params);
    if (err < 0) { fprintf(stderr, "Playback: Unable to set hw params: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    printf("Playback HW params set: Rate=%u, Channels=%u, Format=%s, Period=%lu, Buffer=%lu\n", rate, channels, snd_pcm_format_name(format), period_size, playback_buffer_size);

    // Set Playback SW parameters
    snd_pcm_sw_params_alloca(&playback_sw_params);
    snd_pcm_sw_params_current(playback_handle, playback_sw_params);
    err = snd_pcm_sw_params_set_start_threshold(playback_handle, playback_sw_params, period_size);
    if (err < 0) { fprintf(stderr, "Playback: Error setting start threshold: %s\n", snd_strerror(err)); }
    err = snd_pcm_sw_params_set_avail_min(playback_handle, playback_sw_params, period_size);
    if (err < 0) { fprintf(stderr, "Playback: Error setting avail min: %s\n", snd_strerror(err)); }
    err = snd_pcm_sw_params(playback_handle, playback_sw_params);
    if (err < 0) { fprintf(stderr, "Playback: Error setting sw params: %s\n", snd_strerror(err)); }
    else { printf("Playback SW params set.\n"); }

    // --- Setup Capture Stream ---
    printf("--- Setting up Capture Stream (%s) ---\n", capture_device);
    err = snd_pcm_open(&capture_handle, capture_device, SND_PCM_STREAM_CAPTURE, 0); // Use blocking for simple readi
    if (err < 0) { fprintf(stderr, "Cannot open capture device %s (%s)\n", capture_device, snd_strerror(err)); snd_pcm_close(playback_handle); return 1; }
    printf("Capture device opened.\n");
    snd_pcm_hw_params_alloca(&capture_hw_params);
    err = snd_pcm_hw_params_any(capture_handle, capture_hw_params);
     if (err < 0) { fprintf(stderr, "Capture: Error getting default hw params: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }

    // Set Capture HW parameters (Read Interleaved)
    // Important: Must match playback format, rate, channels for simple mixing
    err = snd_pcm_hw_params_set_access(capture_handle, capture_hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) { fprintf(stderr, "Capture: Error setting access RW_INTERLEAVED: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    err = snd_pcm_hw_params_set_format(capture_handle, capture_hw_params, format);
    if (err < 0) { fprintf(stderr, "Capture: Error setting format %s: %s\n", snd_pcm_format_name(format), snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    unsigned int capture_rate = rate; // Ensure rate matches playback
    err = snd_pcm_hw_params_set_rate_near(capture_handle, capture_hw_params, &capture_rate, 0);
    if (err < 0 || capture_rate != rate) { fprintf(stderr, "Capture: Error setting rate near %u Hz (got %u): %s\n", rate, capture_rate, snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    err = snd_pcm_hw_params_set_channels(capture_handle, capture_hw_params, channels);
    if (err < 0) { fprintf(stderr, "Capture: Error setting channels to %u: %s\n", channels, snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    snd_pcm_uframes_t capture_period_size = period_size; // Use same period size
    err = snd_pcm_hw_params_set_period_size_near(capture_handle, capture_hw_params, &capture_period_size, &dir);
    if (err < 0 || capture_period_size != period_size) { fprintf(stderr, "Capture: Error setting period size near %lu (got %lu): %s\n", period_size, capture_period_size, snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    target_buffer_size = capture_period_size * 4;
    err = snd_pcm_hw_params_set_buffer_size_near(capture_handle, capture_hw_params, &target_buffer_size);
     if (err < 0) { fprintf(stderr, "Capture: Error setting buffer size near %lu: %s\n", target_buffer_size, snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    err = snd_pcm_hw_params_get_buffer_size(capture_hw_params, &capture_buffer_size);
    if (err < 0) { fprintf(stderr, "Capture: Error getting buffer size: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    err = snd_pcm_hw_params(capture_handle, capture_hw_params);
    if (err < 0) { fprintf(stderr, "Capture: Unable to set hw params: %s\n", snd_strerror(err)); snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1; }
    printf("Capture HW params set: Rate=%u, Channels=%u, Format=%s, Period=%lu, Buffer=%lu\n", capture_rate, channels, snd_pcm_format_name(format), capture_period_size, capture_buffer_size);

    // --- Prepare Thread Data ---
    thread_data.playback_handle = playback_handle;
    thread_data.capture_handle = capture_handle;
    thread_data.period_size = period_size;
    thread_data.format = format;
    thread_data.channels = channels;
    thread_data.playback_interleaved = playback_interleaved;

    // Initialize Ring Buffer (e.g., size for 1 second of audio)
    size_t ring_buffer_samples = rate * channels * 1; // 1 second buffer
    thread_data.shared_buffer.buffer = malloc(ring_buffer_samples * sizeof(short));
    if (!thread_data.shared_buffer.buffer) {
        perror("Failed to allocate shared ring buffer");
        snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1;
    }
    thread_data.shared_buffer.size = ring_buffer_samples;
    thread_data.shared_buffer.write_pos = 0;
    thread_data.shared_buffer.read_pos = 0;
    thread_data.shared_buffer.count = 0;
    printf("Shared ring buffer initialized (size = %zu samples)\n", ring_buffer_samples);

    // Initialize Mutex and Condition Variables
    if (pthread_mutex_init(&thread_data.buffer_mutex, NULL) != 0) {
        perror("Mutex initialization failed");
        free(thread_data.shared_buffer.buffer);
        snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1;
    }
    if (pthread_cond_init(&thread_data.cond_not_full, NULL) != 0) {
        perror("Condition variable (not full) initialization failed");
        pthread_mutex_destroy(&thread_data.buffer_mutex); free(thread_data.shared_buffer.buffer);
        snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1;
    }
     if (pthread_cond_init(&thread_data.cond_not_empty, NULL) != 0) {
        perror("Condition variable (not empty) initialization failed");
        pthread_cond_destroy(&thread_data.cond_not_full); pthread_mutex_destroy(&thread_data.buffer_mutex);
        free(thread_data.shared_buffer.buffer);
        snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1;
    }


    // --- Start Threads ---
    printf("Starting threads...\n");
    if (pthread_create(&capture_tid, NULL, capture_thread_func, &thread_data) != 0) {
        perror("Failed to create capture thread");
        snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1;
    }
    if (pthread_create(&playback_tid, NULL, playback_thread_func, &thread_data) != 0) {
        perror("Failed to create playback thread");
        thread_data.running = 0; // Signal capture thread to stop
        pthread_join(capture_tid, NULL);
        snd_pcm_close(playback_handle); snd_pcm_close(capture_handle); return 1;
    }

    // --- Wait for Threads (or signal handling) ---
    printf("Threads started. Press Ctrl+C to stop.\n");
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    // Wait for threads to complete (they will exit when data.running becomes 0)
    pthread_join(playback_tid, NULL);
    printf("Playback thread joined.\n");
    // Ensure capture thread is signaled to stop if playback ends first
    if (thread_data.running) {
        thread_data.running = 0;
        pthread_cond_signal(&thread_data.cond_not_full); // Wake up capture if waiting
    }
    pthread_join(capture_tid, NULL);
    printf("Capture thread joined.\n");


    // --- Cleanup ---
    printf("Attempting cleanup...\n");
    snd_pcm_close(playback_handle);
    snd_pcm_close(capture_handle);
    printf("Audio devices closed.\n");

    // Destroy mutex and condition variables
    pthread_mutex_destroy(&thread_data.buffer_mutex);
    pthread_cond_destroy(&thread_data.cond_not_full);
    pthread_cond_destroy(&thread_data.cond_not_empty);
    printf("Mutex and condition variables destroyed.\n");

    // Free shared buffer
    free(thread_data.shared_buffer.buffer);
    printf("Shared buffer freed.\n");

    printf("Playback finished.\n");

    return 0;
}

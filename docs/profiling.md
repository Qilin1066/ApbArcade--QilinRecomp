# Measuring performance and frame pacing

Enable a report for a normal playing session, then quit normally to write it:

```sh
apb /path/to/roms --profile performance.txt
qilin_run /path/to/roms --frames 6000 --monkey 20261120 --draw 1 --wav audio.wav --audio-threaded 1 --profile headless.txt
```

The report is opt-in. Disabled timing scopes do not read the clock. Reports replace their destination atomically after the audio worker joins. They contain tab-separated stage counts and times in milliseconds, plus the renderer device or headless frame count. Counts, total, mean and maximum cover every sample. Median and nearest-rank p95 use a bounded deterministic reservoir of up to 4,096 samples and become estimates for longer runs. Profiling does not consume the game's random input stream.

| Stage | Interpretation |
| --- | --- |
| simulation_including_video | Time executing between frame callbacks; includes CPU video rendering |
| video_cpu | CPU renderer work inside simulation; do not add it again |
| audio_worker | Worker batch processing, concurrent with simulation |
| audio_submit_and_wait | Submission and any worker backpressure in the player |
| controller_poll | Sampling and converting current controls |
| input_record_io | Writing and flushing the optional input recording |
| color_conversion / upload / menu_ui | CPU time converting pixels, submitting upload and building UI |
| present_and_wait | Presentation calls, potentially including vsync waiting |
| pacing_sleep | Deliberate wait for the board clock |
| frame_interval | Wall time between frame callbacks, excluding paused intervals |

These are CPU/wall-clock measurements, not GPU timestamp queries or physical input-to-photon latency. Overlapping stages must not be summed as a frame cost. Pause/menu redraws are excluded from presentation-stage sampling; entering/resuming a pause resets frame-interval measurement. Headless reports have no window, presentation or physical controller measurements. PNG/hash/WAV/recording work can change throughput; compare runs with the same options and warm up the machine first.

## Development measurement, 20 November 2026

Sequential Release runs of 6,000 seeded frames, without frame hashing or input recording:

| Configuration | Mean simulation time per frame |
| --- | --- |
| CPUs only | 0.325 ms |
| CPUs and video | 0.480 ms |
| CPUs, video and threaded audio | 0.565 ms |

The final run's simulation p95 was 0.855 ms; audio-worker batches averaged 0.193 ms. The board frame period is 16.64 ms. This runner has ample simulation headroom; the numbers do not establish performance on a particular PC, GPU, wheel or display. On the target machine, capture normal gameplay with its actual display mode and controllers and inspect frame intervals alongside presentation waits before choosing an optimisation.

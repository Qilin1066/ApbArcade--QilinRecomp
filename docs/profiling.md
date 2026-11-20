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


## Actual-machine capture

The Windows download includes **Check controls and performance.bat**. It builds/updates the player through the normal launcher and creates a fresh folder under `Hardware checks`. Complete its checklist, play for at least two minutes and quit normally. The folder contains `performance.txt`, `controllers.txt`, `inputs.qir`, stdout/stderr logs and the checklist. Each run gets a unique folder; repeat separately with the R5 and G920. No report is uploaded automatically. Fill in the driver/firmware versions and observations that software cannot infer.

For a source build:

```sh
apb /path/to/roms --record session.qir --profile performance.txt --controller-report controllers.txt
```

The profile/report context includes actual GPU/display/window/vsync/display-lock/effect settings, logical CPUs and RAM, game frames/presents, validation errors and observed audio queue counters. These describe the configuration at exit; note any setting changes in the checklist. The audio counters track stream open attempts, submitted stereo frames, observed empty queues after playback starts, the last/peak observed queued source bytes and the current frequency ratio in parts per million. An observed empty source queue is evidence for investigation, not a measurement of an audible acoustic dropout.

Reports now count intervals above 25 and 50 ms and retain the longest 32, with the game frame at the end of each interval and the simulation wall time inside that gap. Compare that frame during input replay to distinguish a repeatable game workload from presentation, USB/driver or OS stalls. Pauses reset interval measurement. These thresholds flag long gaps around this game's 16.64 ms period, not a universal display-latency standard. Recording itself adds I/O; it has a separate timing stage. A follow-up replay with profiling but no new recording can help isolate that overhead.

The development measurements above establish simulation headroom, not a result on the user's GTX 1080 Ti, display or physical wheel. Those require the actual-machine report and observations. Do not interpret mock-driver or virtual-wheel tests as hardware certification.

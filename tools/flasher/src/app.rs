//! Main application: egui UI + worker thread communication.

use crate::ota;
use eframe::egui;
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver};

// Embedded firmware (only present when `firmware.bin` existed at compile time).
#[cfg(has_bundled_firmware)]
const BUNDLED_FIRMWARE: &[u8] = include_bytes!("../firmware.bin");

// ── Worker channel messages ───────────────────────────────────────────────────

enum Msg {
    Progress(f32),
    Log(String),
    Done(bool),
}

// ── App state ─────────────────────────────────────────────────────────────────

#[derive(PartialEq)]
enum State {
    Idle,
    Flashing,
}

pub struct FlasherApp {
    host:     String,
    fw_path:  Option<PathBuf>,  // only used when firmware is NOT bundled
    state:    State,
    progress: f32,
    log:      String,
    error:    Option<String>,
    rx:       Option<Receiver<Msg>>,
}

impl FlasherApp {
    pub fn new() -> Self {
        #[allow(unused_mut)]
        let mut app = Self {
            host:     "roomba_780.local".to_string(),
            fw_path:  None,
            state:    State::Idle,
            progress: 0.0,
            log:      String::new(),
            error:    None,
            rx:       None,
        };

        // When firmware is not bundled, auto-detect firmware.bin next to the executable.
        #[cfg(not(has_bundled_firmware))]
        if let Ok(exe) = std::env::current_exe() {
            let fw = exe.with_file_name("firmware.bin");
            if fw.exists() {
                app.fw_path = Some(fw);
            }
        }

        app
    }

    // ── Firmware resolution ───────────────────────────────────────────────────

    fn get_firmware(&self) -> Option<Vec<u8>> {
        #[cfg(has_bundled_firmware)]
        return Some(BUNDLED_FIRMWARE.to_vec());

        #[cfg(not(has_bundled_firmware))]
        self.fw_path.as_ref().and_then(|p| std::fs::read(p).ok())
    }

    // ── Flash trigger ─────────────────────────────────────────────────────────

    fn start_flash(&mut self) {
        let host = self.host.trim().to_string();
        if host.is_empty() {
            self.error = Some("Enter the device IP or hostname.".into());
            return;
        }
        let firmware = match self.get_firmware() {
            Some(fw) => fw,
            None => {
                self.error = Some("No firmware found. Select a .bin file.".into());
                return;
            }
        };

        self.error = None;
        self.log.clear();
        self.progress = 0.0;
        self.state = State::Flashing;
        self.log.push_str(&format!("Starting update on {host}...\n"));

        let (tx, rx) = mpsc::channel();
        self.rx = Some(rx);

        std::thread::spawn(move || {
            let tx_progress = tx.clone();
            let tx_log = tx.clone();

            let result = ota::flash(
                &host,
                8266,
                "",
                "firmware.bin",
                &firmware,
                move |p| {
                    let _ = tx_progress.send(Msg::Progress(p));
                },
                move |s| {
                    let _ = tx_log.send(Msg::Log(s));
                },
            );

            if let Err(ref e) = result {
                let _ = tx.send(Msg::Log(format!("Error: {e}\n")));
            }
            let _ = tx.send(Msg::Done(result.is_ok()));
        });
    }

    // ── Drain worker channel ──────────────────────────────────────────────────

    fn poll(&mut self) {
        // Collect all pending messages first so we release the borrow on self.rx
        // before we potentially assign None to it in the Done branch.
        let msgs: Vec<Msg> = {
            let Some(rx) = &self.rx else { return };
            std::iter::from_fn(|| rx.try_recv().ok()).collect()
        };
        for msg in msgs {
            match msg {
                Msg::Progress(p) => self.progress = p,
                Msg::Log(s)      => self.log.push_str(&s),
                Msg::Done(ok)    => {
                    if ok {
                        self.progress = 1.0;
                        self.log
                            .push_str("\nUpdate completed. Device will reboot automatically.\n");
                    } else {
                        self.log.push_str("\nUpdate failed. See log above.\n");
                    }
                    self.state = State::Idle;
                    self.rx = None;
                }
            }
        }
    }
}

// ── egui render ───────────────────────────────────────────────────────────────

impl eframe::App for FlasherApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll();

        // Keep the UI refreshing while a flash is in progress.
        if self.state == State::Flashing {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }

        let flashing = self.state == State::Flashing;

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(4.0);
            ui.vertical_centered(|ui| {
                ui.heading("Firmware Updater");
            });
            ui.add_space(8.0);

            // ── Input fields ──────────────────────────────────────────────────
            egui::Grid::new("fields")
                .num_columns(2)
                .spacing([8.0, 6.0])
                .show(ui, |ui| {
                    ui.label("Device (IP or name):");
                    ui.add_enabled(
                        !flashing,
                        egui::TextEdit::singleline(&mut self.host).desired_width(220.0),
                    );
                    ui.end_row();

                    // File picker — only shown when firmware is not bundled.
                    #[cfg(not(has_bundled_firmware))]
                    {
                        ui.label("Firmware file:");
                        ui.horizontal(|ui| {
                            let display = self
                                .fw_path
                                .as_deref()
                                .and_then(|p| p.to_str())
                                .unwrap_or("")
                                .to_string();
                            let mut display = display;
                            ui.add_enabled(
                                false, // read-only path display
                                egui::TextEdit::singleline(&mut display).desired_width(170.0),
                            );
                            if !flashing && ui.button("Browse…").clicked() {
                                if let Some(path) = rfd::FileDialog::new()
                                    .add_filter("Firmware binary", &["bin"])
                                    .pick_file()
                                {
                                    self.fw_path = Some(path);
                                    self.error = None;
                                }
                            }
                        });
                        ui.end_row();
                    }
                });

            ui.add_space(8.0);

            // ── Flash button ──────────────────────────────────────────────────
            ui.vertical_centered(|ui| {
                let label = if flashing { "Updating…" } else { "Update firmware" };
                if ui
                    .add_enabled(
                        !flashing,
                        egui::Button::new(label).min_size(egui::vec2(160.0, 28.0)),
                    )
                    .clicked()
                {
                    self.start_flash();
                }
                if let Some(ref err) = self.error {
                    ui.colored_label(egui::Color32::RED, err);
                }
            });

            ui.add_space(8.0);

            // ── Progress bar ──────────────────────────────────────────────────
            ui.horizontal(|ui| {
                ui.label("Progress:");
                let bar_width = ui.available_width();
                ui.add(
                    egui::ProgressBar::new(self.progress)
                        .desired_width(bar_width)
                        .show_percentage(),
                );
            });

            ui.add_space(4.0);

            // ── Log ───────────────────────────────────────────────────────────
            ui.label("Log:");
            let available_height = ui.available_height() - 4.0;
            egui::ScrollArea::vertical()
                .max_height(available_height)
                .stick_to_bottom(true)
                .show(ui, |ui| {
                    let text_width = ui.available_width();
                    ui.add(
                        egui::TextEdit::multiline(&mut self.log)
                            .font(egui::TextStyle::Monospace)
                            .desired_width(text_width)
                            .interactive(false),
                    );
                });
        });
    }
}

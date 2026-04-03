mod app;
mod ota;

fn main() {
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_title("RoombaFlasher")
            .with_inner_size([520.0, 400.0])
            .with_resizable(false),
        ..Default::default()
    };
    eframe::run_native(
        "RoombaFlasher",
        options,
        Box::new(|_cc| Ok(Box::new(app::FlasherApp::new()))),
    )
    .expect("Failed to start UI");
}

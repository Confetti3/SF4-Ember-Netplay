use ember_reports::{App, config::Config, router};
use std::process::ExitCode;

fn main() -> ExitCode {
    // A third-party parser panic must not print player data via Rust's default
    // panic hook. The request worker catches the panic and emits raw facts.
    std::panic::set_hook(Box::new(|_| {
        eprintln!("method=- status=500 size=0 report_id=- processing_ms=0");
    }));
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("ember-reports: {message}");
            ExitCode::FAILURE
        }
    }
}
fn run() -> Result<(), &'static str> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let path = match args.as_slice() {
        [] => std::path::PathBuf::from("/etc/ember-reports/config.json"),
        [path] => path.into(),
        _ => return Err("usage: ember-reports [/etc/ember-reports/config.json]"),
    };
    let bytes = std::fs::read(path).map_err(|_| "cannot read configuration")?;
    let config: Config =
        serde_json::from_slice(&bytes).map_err(|_| "invalid configuration JSON")?;
    config.validate()?;
    let key = Config::credential()?;
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .max_blocking_threads(4)
        .enable_all()
        .build()
        .map_err(|_| "cannot start runtime")?;
    runtime.block_on(async {
        let address = config.bind;
        let app = App::new(config, &key).await?;
        let listener = tokio::net::TcpListener::bind(address)
            .await
            .map_err(|_| "cannot bind listener")?;
        let background = tokio::spawn(app.clone().background());
        let result = axum::serve(
            listener,
            router(app).into_make_service_with_connect_info::<std::net::SocketAddr>(),
        )
        .with_graceful_shutdown(shutdown())
        .await
        .map_err(|_| "HTTP server failed");
        background.abort();
        result
    })
}
async fn shutdown() {
    #[cfg(unix)]
    {
        if let Ok(mut terminate) =
            tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
        {
            tokio::select! { _ = terminate.recv() => {}, _ = tokio::signal::ctrl_c() => {} }
            return;
        }
    }
    let _ = tokio::signal::ctrl_c().await;
}

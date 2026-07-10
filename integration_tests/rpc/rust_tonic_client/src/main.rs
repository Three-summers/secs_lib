mod checks;
mod harness;
mod items;
mod scenarios;

mod rpc {
    tonic::include_proto!("secs.rpc.v1");
}

use std::{
    ffi::OsString,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};

#[derive(Debug)]
struct Args {
    server: PathBuf,
    peer: PathBuf,
}

impl Args {
    fn parse() -> Result<Self> {
        let mut server = None;
        let mut peer = None;
        let mut args = std::env::args_os().skip(1);

        while let Some(arg) = args.next() {
            match arg.to_str() {
                Some("--server") => server = Some(required_value(&mut args, "--server")?),
                Some("--peer") => peer = Some(required_value(&mut args, "--peer")?),
                Some(other) => bail!("unknown option: {other}"),
                None => bail!("arguments must be valid UTF-8"),
            }
        }

        let server = server.context("missing required option --server <path>")?;
        let peer = peer.context("missing required option --peer <path>")?;
        ensure_executable(&server, "RPC server")?;
        ensure_executable(&peer, "HSMS peer")?;
        Ok(Self { server, peer })
    }
}

fn required_value(args: &mut impl Iterator<Item = OsString>, option: &str) -> Result<PathBuf> {
    args.next()
        .map(PathBuf::from)
        .with_context(|| format!("missing value for {option}"))
}

fn ensure_executable(path: &Path, what: &str) -> Result<()> {
    if !path.is_file() {
        bail!("{what} executable not found: {}", path.display());
    }
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse()?;
    scenarios::run_contract_and_validation(&args).await?;
    scenarios::run_lifecycle_and_messaging(&args).await?;
    scenarios::run_fault_and_concurrency_tests(&args).await?;
    println!("rust tonic RPC integration ok");
    Ok(())
}

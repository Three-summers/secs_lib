use std::{env, path::PathBuf};

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../../..");
    let proto_root = root.join("src/rpc/proto");
    let proto = proto_root.join("secs/rpc/v1/secs_rpc.proto");

    println!("cargo:rerun-if-changed={}", proto.display());
    tonic_prost_build::configure()
        .build_server(false)
        .compile_protos(&[proto], &[proto_root])
        .expect("compile secs RPC proto");
}

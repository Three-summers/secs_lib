use crate::rpc::{ItemNode, ItemType};

fn empty(kind: ItemType) -> ItemNode {
    ItemNode {
        r#type: Some(kind as i32),
        items: Vec::new(),
        ascii_value: None,
        binary_value: None,
        bool_values: Vec::new(),
        i1_values: Vec::new(),
        i2_values: Vec::new(),
        i4_values: Vec::new(),
        i8_values: Vec::new(),
        u1_values: Vec::new(),
        u2_values: Vec::new(),
        u4_values: Vec::new(),
        u8_values: Vec::new(),
        f4_values: Vec::new(),
        f8_values: Vec::new(),
    }
}

pub fn ascii(value: impl Into<String>) -> ItemNode {
    let mut node = empty(ItemType::Ascii);
    node.ascii_value = Some(value.into());
    node
}

pub fn u4(values: Vec<u32>) -> ItemNode {
    let mut node = empty(ItemType::U4);
    node.u4_values = values;
    node
}

pub fn list(items: Vec<ItemNode>) -> ItemNode {
    let mut node = empty(ItemType::List);
    node.items = items;
    node
}

pub fn ping(sequence: u32) -> ItemNode {
    list(vec![ascii("PING"), u4(vec![sequence])])
}

pub fn all_types() -> ItemNode {
    let mut binary = empty(ItemType::Binary);
    binary.binary_value = Some(vec![0, 1, 127, 128, 255]);

    let mut boolean = empty(ItemType::Boolean);
    boolean.bool_values = vec![false, true, true, false];

    let mut i1 = empty(ItemType::I1);
    i1.i1_values = vec![-128, -1, 0, 127];
    let mut i2 = empty(ItemType::I2);
    i2.i2_values = vec![-32768, -1, 0, 32767];
    let mut i4 = empty(ItemType::I4);
    i4.i4_values = vec![i32::MIN, -1, 0, i32::MAX];
    let mut i8 = empty(ItemType::I8);
    i8.i8_values = vec![i64::MIN, -1, 0, i64::MAX];

    let mut u1 = empty(ItemType::U1);
    u1.u1_values = vec![0, u8::MAX as u32];
    let mut u2 = empty(ItemType::U2);
    u2.u2_values = vec![0, u16::MAX as u32];
    let mut u8 = empty(ItemType::U8);
    u8.u8_values = vec![0, u64::MAX];

    let mut f4 = empty(ItemType::F4);
    f4.f4_values = vec![-1.5, 0.0, 2.5];
    let mut f8 = empty(ItemType::F8);
    f8.f8_values = vec![-1.25, 0.0, 3.5];

    list(vec![
        ascii("ALL-TYPES"),
        binary,
        boolean,
        i1,
        i2,
        i4,
        i8,
        u1,
        u2,
        u4(vec![0, u32::MAX]),
        u8,
        f4,
        f8,
        list(vec![ascii("NESTED"), u4(vec![7, 11])]),
    ])
}

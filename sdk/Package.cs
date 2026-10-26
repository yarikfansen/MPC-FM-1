// SPDX-License-Identifier: MIT
// Copyright (c) 2023 Andrey Grigoryev (kagaimiq), 2026 M-VAVE.
// JieLi formats and ciphers: jl-misctools 0a5b12d. FM1 layout and writer: M-VAVE.
// Full MIT terms and source links: THIRD_PARTY.md.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace FM1 {
public static class Package {
    const int Area = 0x4000, EntryPoint = 0x02000120, AppData = 0x120;
    const int FlashLimit = 0xff000, UserSize = 200704, BtSize = 4096, MinimumVm = 32768;
    const ushort PublicCode = 0x980f;
    static readonly ushort[] CrcTable = MakeCrcTable();
    static ushort[] MakeCrcTable() {
        ushort[] table = new ushort[256];
        for (int i = 0; i < 256; i++) {
            int value = i << 8;
            for (int b = 0; b < 8; b++) value = (value << 1) ^ ((value & 32768) == 0 ? 0 : 0x1021);
            table[i] = (ushort)value;
        }
        return table;
    }
    static ushort Crc(byte[] bytes, int begin, int length) {
        ushort value = 0;
        for (int end = begin + length; begin < end; begin++) value = (ushort)((value << 8) ^ CrcTable[(value >> 8) ^ bytes[begin]]);
        return value;
    }
    static void Cipher(byte[] bytes, int begin, int length, int state) {
        for (int end = begin + length; begin < end; begin++) {
            bytes[begin] ^= (byte)state;
            state = ((state << 1) ^ ((state & 32768) == 0 ? 0 : 0x1021)) & 65535;
        }
    }
    static int RoundUp(int size, int alignment) {return checked((size + alignment - 1) & -alignment);}
    sealed class Image {
        internal readonly byte[] Bytes;
        internal Image(int size, byte fill) {Bytes = new byte[size];if (fill != 0) for (int n = 0; n < size; n++) Bytes[n] = fill;}
        internal void Number(int offset, uint value, int width) {for (int n = 0; n < width; n++) Bytes[offset+n] = (byte)(value >> (8*n));}
        internal void Copy(int offset, byte[] data) {Buffer.BlockCopy(data,0,Bytes,offset,data.Length);}
        internal void Text(int offset, string value) {Copy(offset,Encoding.ASCII.GetBytes(value));}
        internal void Checksum(int field, int offset, int size) {Number(field,Crc(Bytes,offset,size),2);}
        internal void Record(int offset, string name, ushort crc, uint address, uint size, ushort attributes, ushort last, uint? extra) {
            Array.Clear(Bytes,offset,32);
            Number(offset+2,crc,2);Number(offset+4,address,4);Number(offset+8,size,4);
            Number(offset+12,attributes,2);Number(offset+14,last,2);
            for (int n = 16; n < 32; n++) Bytes[offset+n] = 255;
            Text(offset+16,name);Bytes[offset+16+name.Length] = 0;
            if (extra.HasValue) Number(offset+28,extra.Value,4);
            Checksum(offset,offset+2,30);
        }
    }
    static byte[] ReadResource(string directory, string name, int size) {
        byte[] bytes = File.ReadAllBytes(Path.Combine(directory,name));
        if (bytes.Length != size) throw new InvalidDataException("Unexpected FM1 resource: " + name);
        return bytes;
    }
    static byte[] EncodeBoot(byte[] plain) {
        byte[] result = (byte[])plain.Clone();
        int banks = BitConverter.ToUInt16(plain,0);
        if (banks < 1 || banks > plain.Length/16) throw new InvalidDataException("Invalid boot directory.");
        int previousEnd = banks*16;
        for (int i = 0; i < banks; i++) {
            int header = i*16,length = BitConverter.ToUInt16(plain,header+2),start = BitConverter.ToInt32(plain,header+8);
            if (length == 0 || start < previousEnd || start > plain.Length-length ||
                Crc(plain,header,14) != BitConverter.ToUInt16(plain,header+14) ||
                Crc(plain,start,length) != BitConverter.ToUInt16(plain,header+12)) throw new InvalidDataException("Invalid boot bank.");
            Cipher(result,header,16,65535);Cipher(result,start,length,65535);previousEnd = start+length;
        }
        return result;
    }
    static Image BuildFlash(byte[] application, string directory, int end, int vmEnd) {
        byte[] boot = ReadResource(directory,"uboot.boot",14488);
        byte[] config = ReadResource(directory,"boot-config.bin",721);
        byte[] tool = ReadResource(directory,"cfg_tool.bin",383);
        Image image = new Image(end,255),settings = new Image(34+config.Length,0);
        // Deterministic jl-misctools chipkeybin descriptor for the public image.
        for (int bit = 0; bit < 16; bit++) {
            settings.Bytes[bit] = 16;
            settings.Bytes[16+bit] = (byte)(16 ^ (((PublicCode >> bit)&1) == 0 ? 255 : 0));
        }
        settings.Checksum(32,0,32);settings.Copy(34,config);
        // AC791N boot header fields from the verified FM1 boot resources.
        image.Copy(0,new byte[] {0,0,0x20,2,0x2f,0x10,0x4c,0xc9,0,0xf0,0x0f,0,0x10,0x10,0xa4,0xff,
            0x4e,0x5d,0x0b,0x41,0xe0,0xcd,0x78,0x1d,0xc8,0x56,0x41,0x7f,0xb3,0x67,0xcf,0x9f});
        image.Checksum(0,2,30);
        int configAt = 160+boot.Length;
        image.Record(32,"uboot.boot",Crc(boot,0,boot.Length),160,(uint)boot.Length,0,0,null);
        image.Record(64,"isd_config.ini",Crc(settings.Bytes,0,settings.Bytes.Length),(uint)configAt,(uint)settings.Bytes.Length,0x8002,0,null);
        image.Record(96,"app_dir_head",65535,Area,uint.MaxValue,0xff81,0,null);
        image.Record(128,"key_mac",65535,FlashLimit,4096,0x0112,65535,null);
        for (int p = 0; p < 160; p += 32) Cipher(image.Bytes,p,32,65535);
        image.Copy(160,EncodeBoot(boot));image.Copy(configAt,settings.Bytes);
        int payloadSize = AppData+application.Length+tool.Length;
        int app = Area+AppData;
        image.Copy(app,application);image.Copy(app+application.Length,tool);
        image.Record(Area+32,"app.bin",Crc(application,0,application.Length),AppData,(uint)application.Length,0xff82,0,null);
        image.Record(Area+64,"cfg_tool.bin",Crc(tool,0,tool.Length),(uint)(AppData+application.Length),(uint)tool.Length,0xff82,0,null);
        image.Record(Area+96,"VM",65535,(uint)end,(uint)(vmEnd-end),0x8112,0,MinimumVm);
        image.Record(Area+128,"PRCT",65535,0,(uint)end,0x8292,0,0x45444f43);
        image.Record(Area+160,"BTIF",65535,(uint)vmEnd,BtSize,0x8192,1,0x4f545541);
        image.Record(Area,"app_area_head",Crc(image.Bytes,Area+32,payloadSize-32),EntryPoint,(uint)payloadSize,0xff83,0,null);
        int reserved = Area+payloadSize;
        image.Record(reserved+32,"USER",65535,FlashLimit-UserSize,UserSize,0x8192,1,0x4f545541);
        image.Record(reserved,"EXT_RESERVED",Crc(image.Bytes,reserved+32,32),32,64,0xff93,1,null);
        for (int p = 0; p < payloadSize+64; p += 32)
            Cipher(image.Bytes,Area+p,Math.Min(32,payloadSize+64-p),PublicCode^(p>>2));
        return image;
    }
    static byte[] Wrap(Image flash, byte[] loader, string product) {
        if (loader.Length < 1 || loader.Length >= 32768) throw new InvalidDataException("Invalid FM1 update loader.");
        int loaderAt = 1024+RoundUp(flash.Bytes.Length,32),total = loaderAt+RoundUp(loader.Length,32);
        Image ufw = new Image(total,255);
        Array.Clear(ufw.Bytes,0,224);
        byte[][] files = {flash.Bytes,loader};string[] names = {"flash.bin","ota.bin"};
        for (int i = 0; i < files.Length; i++) {
            int record = 64+i*80;
            ufw.Number(record,(uint)(i == 0 ? 0 : 100),2);ufw.Number(record+2,(uint)i,2);
            ufw.Number(record+4,Crc(files[i],0,files[i].Length),2);
            ufw.Number(record+8,(uint)(i == 0 ? 1024 : loaderAt),4);
            ufw.Number(record+12,(uint)files[i].Length,4);ufw.Number(record+16,(uint)RoundUp(files[i].Length,32),4);
            ufw.Text(record+64,names[i]);Cipher(ufw.Bytes,record,80,65535);
        }
        ufw.Number(4,(uint)total,4);ufw.Number(8,2,2);ufw.Number(10,4,2);ufw.Number(12,512,4);
        ufw.Text(16,"AC791N");ufw.Checksum(2,64,160);ufw.Checksum(0,2,62);Cipher(ufw.Bytes,0,64,65535);
        ufw.Copy(1024,flash.Bytes);ufw.Copy(loaderAt,loader);
        byte[] output = new byte[total+20];
        for (int source = 0,destination = 0; source < total; source++,destination++) {
            output[destination] = ufw.Bytes[source];
            if (source < 940 && (source+1)%47 == 0) {
                int index = source/47;
                output[++destination] = index < product.Length ? (byte)(product[index]+index+1) : (byte)125;
            }
        }
        return output;
    }
    public static Dictionary<string,object> Create(byte[] app, string resources, string output, int userBytes, string product) {
        if (product == null || !System.Text.RegularExpressions.Regex.IsMatch(product,"\\AFM-1_8[0-9]{6}\\z"))
            throw new InvalidDataException("Invalid FM1 update identity.");
        if (app == null || app.Length == 0 || app.Length > FlashLimit-UserSize-BtSize-MinimumVm-Area-AppData-383-64 || userBytes != UserSize)
            throw new InvalidDataException("Application does not fit the FM1 layout.");
        int imageSize = RoundUp(Area+AppData+app.Length+383+64,4096),userAt = FlashLimit-UserSize,vmEnd = userAt-BtSize;
        if (imageSize > vmEnd-MinimumVm) throw new InvalidDataException("Insufficient FM1 settings space.");
        Image image = BuildFlash(app,resources,imageSize,vmEnd);
        byte[] package = Wrap(image,File.ReadAllBytes(Path.Combine(resources,"ota.bin")),product);
        File.WriteAllBytes(output,package);
        return new Dictionary<string,object> {
            {"flash_bytes",1048576},{"board_access",false},{"fits",true},{"packager","source"},
            {"FLASH_BIN_SIZE",imageSize},{"FLASH_NEED_SIZE",FlashLimit},{"VM_START_ADDR",imageSize},{"VM_END_ADDR",vmEnd},
            {"VM_REAL_SIZE",vmEnd-imageSize},{"BTIF_RESERVED_START",vmEnd},{"BTIF_RESERVED_END",userAt},
            {"USER_RESERVED_START",userAt},{"USER_RESERVED_END",FlashLimit},{"vm_minimum_bytes",MinimumVm},
            {"vm_extra_gap_bytes",vmEnd-imageSize-MinimumVm},{"btif_bytes",BtSize},{"user_bytes",UserSize}
        };
    }
}
}

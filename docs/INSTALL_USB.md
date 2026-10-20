# Install MihawkOS's Windows runtime from a USB drive

This is the page the QR code in MihawkOS's setup opens. Follow it on your phone while the PS5 shows "Prepare a USB
drive".

> [!NOTE]
> The `mihawkos` folder comes with each MihawkOS release, as the `…-usb-mihawkos.zip` asset on the repository's
> Releases page. Unzip it and copy the `mihawkos` folder to the top of the drive.

## What you need

- A USB drive of at least 1 GB. Its contents will be erased.
- A computer (Windows, macOS or Linux), only to prepare the drive.

## 1. Format the drive: exFAT, MBR

The PS5 reads exFAT or FAT32 drives with an **MBR** partition table. It does not read GPT or NTFS drives: a GPT drive
stays invisible, and the setup says "No USB drive found".

**Windows**

1. Plug the drive in. Open **Disk Management** (right-click Start, then Disk Management).
2. If the drive's disk shows **GPT**: right-click the disk, delete its volumes, then **Convert to MBR Disk**.
3. Right-click the empty space, then **New Simple Volume**. Choose **exFAT** and finish.

**macOS**

1. Open **Disk Utility**, then View, then **Show All Devices**.
2. Select the drive itself, not a volume under it, and choose **Erase**.
3. Format: **ExFAT**. Scheme: **Master Boot Record**. Then **Erase**.

**Linux**

```bash
lsblk
```

Find the drive, for example `/dev/sdX`. Check twice: the next commands erase it.

```bash
sudo parted /dev/sdX --script mklabel msdos mkpart primary 1MiB 100%
```

```bash
sudo mkfs.exfat /dev/sdX1
```

## 2. Copy the folder

1. Download MihawkOS and unpack it.
2. Copy its `mihawkos` folder to the top of the drive, not inside another folder: the drive shows `mihawkos` as soon
   as you open it.
3. Eject the drive before you unplug it.

## 3. Plug it into the PS5

Use any USB port on the PS5. Wait a few seconds, then choose **Check drive**. When the setup says the drive holds about
1.2 GB to install, choose **Install**. The install takes about a minute. What it has installed is kept if anything
stops it, so **Try again** carries on.

## When the setup says...

| The setup says | Do this |
| --- | --- |
| No USB drive found | Plug it in and wait a few seconds. If it is in, format it as exFAT with MBR (step 1). |
| The drive has no mihawkos folder at its top | Copy the folder to the top of the drive (step 2). |
| The mihawkos folder is incomplete | Copy the whole folder again; the copy was cut short. |
| A file on the drive is damaged or from another version | Copy the folder again from the same MihawkOS download as the title. |
| Your PS5 needs ... free | Delete something on the PS5, then try again. |
| The PS5's storage refused a write | Try again. If it happens again, restart the PS5. |

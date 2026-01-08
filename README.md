# Nextui Music Player
A comprehensive music playback application for NextUI featuring local file playback, internet radio streaming, and MP3 audio downloading.

This pak is designed and tested only for the NextUI and TrimUI Brick @ Trimui Brick Hammer:

## Installation

1. Mount your NextUI SD card to a computer.
2. Download the latest release from Github. It will be named `Music.Player.pak.zip`.
3. Copy the zip file to `/Tools/tg5040/Music.Player.pak.zip`.
4. Extract the zip in place, then delete the zip file.
5. Confirm that there is a /Tools/tg5040/Music Player.pak folder on your SD card.
6. Unmount your SD Card and insert it into your TrimUI device.

## Features

### Local Music Playback
- Supports WAV, MP3, OGG, and FLAC formats
- File browser for navigating music libraries (Audio files must be placed in ./Music folder)
- Shuffle and repeat modes
- Volume control
- Waveform overview for track progress

### Internet Radio
- Preset station management (add, remove, save)
- Curated station browser organized by country
- Support for MP3 and AAC streams
- Direct streaming (Shoutcast/Icecast) and HLS (m3u8) support
- HTTPS support via mbedTLS
- ICY metadata display (song title, artist, station info)

### MP3 Downloader
- Search YouTube for music
- Download queue management
- Batch downloading with progress tracking
- yt-dlp version management and updates
- Downloaded files integrate with local music library

## Controls

### Main Menu Navigation
- **D-Pad**: Navigate menus and file browser
- **A Button**: Select/Confirm
- **B Button**: Back/Cancel/Exit
- **Start**: Exit Application
- **Select**: Toggle visualization mode
- **L/R Shoulders**: Adjust volume

### Music Player
- **A Button**: Play/Pause
- **B Button**: Back/Cancel/Exit
- **X Button**: Toggle Shuffle
- **Y Button**: Toggle Repeat
- **D-Pad Up**: Next Track
- **D-Pad Down**: Prev Track
- **D-Pad Right**: Fast Foward
- **D-Pad Left**: Rewind
- **Start**: Exit Application

### Radio Player
- **B Button**: Back/Stop
- **D-Pad Up**: Next Station
- **D-Pad Down**: Prev Station
- **Start**: Exit Application

## Usage

### Playing Local Music
- Navigate to your music folder using the `Local File` menu
- Select a file to start playback

![IMG_32041](https://github.com/user-attachments/assets/23e9fbbd-4e97-4463-86ab-710d794c4588)

### Internet Radio
- Navigate to the stations list using the `Internet Radio` menu
- Select from predefined list of station in the `Manage Stations` menu
- Or add custom stations at `.userdata/shared/radio_stations.txt`
- Metadata displays automatically when available

![IMG_32061](https://github.com/user-attachments/assets/4b31972b-1408-43be-bdc7-400f43a2e1cf)


### MP3 Downloads
- Navigate to the music search page using the `MP3 Downloader` menu
- Enter search query using on-screen keyboard
- Select tracks to add to download queue
- Start the queue in `Download Queue` page.
- Downloaded audio will be available in `Local File` menu

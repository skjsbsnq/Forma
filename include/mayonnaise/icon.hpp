#pragma once

#include <cstdint>

namespace mayo {

/// Built-in line icons, drawn as vectors in the spirit of (but not copied from) SF Symbols.
enum class Icon : std::uint16_t {
    None,
    Checkmark, Xmark, Plus, Minus, Ellipsis,
    ChevronLeft, ChevronRight, ChevronUp, ChevronDown, ChevronUpDown,
    ArrowLeft, ArrowRight, ArrowUp, ArrowDown,
    Magnifier, Gear, Sidebar, Info, Bell, Person, PersonCircle, Wifi, Bluetooth, Globe,
    Moon, Sun, Lock, Paintbrush, Display, Battery, Speaker, Heart, Star, Folder, Document,
    Trash, Share, House, Clock, Hand, Keyboard, Photo, Square, Circle, Grid, List, Bolt,
    Cloud, Envelope, Calendar, Pencil, HeartFill, Import, ZoomIn, ZoomOut,
    Play, Pause, SkipBack, SkipForward, Backward, Forward, Expand, Contract, SpeakerSlash, Film, Repeat,
    ArrowDownCircle, MusicNote, HardDrive, Eject, Columns, Tag, DocOnDoc, FolderPlus, ArrowUpRight, Terminal,
    Mouse, Accessibility, Network, Windows, Hourglass, Shield, Printer, Sparkles, Captions, Library,
    Shuffle, RepeatOne, Album, UsbDrive,
};

}  // namespace mayo

///*
// * Copyright (c) 2019-2020, Jesse Buhagiar <jooster669@gmail.com>
// * All rights reserved.
// *
// * Redistribution and use in source and binary forms, with or without
// * modification, are permitted provided that the following conditions are met:
// *
// * 1. Redistributions of source code must retain the above copyright notice, this
// *    list of conditions and the following disclaimer.
// *
// * 2. Redistributions in binary form must reproduce the above copyright notice,
// *    this list of conditions and the following disclaimer in the documentation
// *    and/or other materials provided with the distribution.
// *
// * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// */
//
////
//// Intel 82078 Floppy Disk controller driver
//// Datasheet: https://wiki.qemu.org/images/f/f0/29047403.pdf
////
//// The Intel 82078 is a 44-pin package, CHMOS Single Chip Floppy Disk Controller found commonly
//// on later PCs in the mid to late 90s. It supports a multitude of floppy drives found in computers
//// at the time, up to and including 2.8MB ED Floppy Disks and is software compatible with previous FDCs.
//// Drive in this case refers to the actual drive where the media is inserted and a disk is the actual
//// magnetic floppy disk media. This controller is emulated by QEMU.
////
//// Certain terminology exists in the code of this driver that may be confusing, being that there
//// is a lot of code and documentation online that is seemingly conflicting. I've used terms found
//// directly in the datasheet however for the sake of completeness I'll explain them here:
////
////      - Cylinder: One full circular 'slice' of the floppy disk. It contains 18 sectors
////                  on a 3.5" floppy disk. It is also known as a 'track'. There are
////                  80 tracks on a single side of a floppy disk.
////      - Sector:   One 512 byte chunk of a track.
////      - Head:     The read write arm found inside the drive itself. On a double sided
////                  floppy disk drive, there are two, one for the top tracks of the disk
////                  and the other for the bottom tracks.
////      - CHS:      Cylinder, Head, Sector. The addressing type this floppy controller
////                  uses to address the disk geometry.
////
//// A normal PC System usually contains one or two floppy drives. This controller contains the
//// ability to control up to four drives with the one controller, however it is very rare for
//// most systems to contain this amount of drives.
////
//// The basic operation of the drive involves reseting the drive in hardware, then sending command
//// bytes to the FIFO, allowing the command to execute, then flushing the FIFO by reading `n` bytes
//// from it. Most commands are multi-parameter and multi-result, so it's best to consult the datasheet
//// from page 23. It is recommended that a SENSE command is performed to retrieve valubable interrupt
//// information about the performed action.
////
//// Reseting the controller involves the following:
////      - Acquire the version ID of the controller.
////      - Reset the DOR register
////      - Deassert software reset bit in the DOR register and assert the DMAGATE pin to initialize DMA mode
////      - Program the Configuration Control Register (CCR) for 3.5" 1.44MB diskettes
////      - Send a SPECIFY command to specify more drive information. Refer to the datasheet
////
//// The drive (being mapped to the controller) will then be in a state that will accept the correct media.
//// The DMA controller is also set up here, which is on channel 2. This only needs to be done once, the
//// read and write commands can toggle the appropriate bits themselves to allow a specific transfer direction.
////
//// Recalibrating the drive refers to the act of resetting the head of the drive back to track/cylinder 0. It
//// is essentially the same as a seek, however returning the drive to a known position. For the sake of brevity,
//// only the recalibrate sequence will be described.
////
////      - Enable the drive and it's motor (all drive motors are manually enabled by us!).
////      - Issue a recalibrate or a seek command
////      - Wait for interrupt
////      - Issue a SENSE command, letting the drive know we handled the interrupt
////      - Flush the FIFO and check the cylinder value to ensure we are at the correct spot.
////
//// Once this has been completed, the drive will either be at the desired position or back at cylinder 0.
////
//// To perform a READ or a WRITE of the diskette inserted, the following actions must be taken:
////
////      -The drive and it's motor must be enabled
////      -The data rate must be set via CCR
////      -The drive must be then recalibrated to ensure the head has not drifted.
////      -A wait of 500ms or greater must occur to allow the drive to spin up from inertia.
////      -The DMA direction of the transfer is then configured.
////      -The READ or WRITE command is issued to the controller.
////      -A timeout counter is started. This is only for real hardware and is currently not implemented.
////      -Read the result bytes.
////      -Attempt to READ or WRITE to the disk. Intel recommends doing this a max of 3 times before failing.
////
////
////
//#pragma once
//
////#include <Kernel/Devices/BlockDevice.h>
////#include <Kernel/Interrupts/IRQHandler.h>
////#include <Kernel/Lock.h>
////#include <Kernel/VM/PhysicalPage.h>
////#include <LibBareMetal/Memory/PhysicalAddress.h>
//
//#include <sys/types.hpp>
//#include <devices/BlockDevice.hpp>
//#include <kernel/semaphore.hpp>
//#include <arch/x86/interrupt.hpp>
//
//namespace kernel {
//    class Frame;
//
//    struct FloppyControllerCommand {
//        uint8_t cmd;         // Command to send to the controller
//        uint8_t numParams;   // Number of parameters to send to the drive
//        uint8_t numReturned; // Number of values we expect to be returned by the command
//        uint8_t *params;
//        uint8_t *result;
//    };
//
////
//// NOTE: This class only supports 3.5" 1.44MB floppy disks!
//// Any other type of drive will be ignored
////
//// Also not that the floppy disk controller is set up to be in PS/2 mode, which
//// uses the Intel 82077A controller. More about this controller can
//// be found here: http://www.buchty.net/casio/files/82077.pdf
////
//    class FloppyDisk final : public BlockDevice, public x86::IRQHandler {
//        static constexpr uint8_t SECTORS_PER_CYLINDER = 18;
//        static constexpr uint8_t CYLINDERS_PER_HEAD = 80;
//        static constexpr uint16_t BYTES_PER_SECTOR = 512;
//
//    public:
//        //
//        // Is this floppy drive the master or the slave on the controller??
//        //
//        enum class DriveType : uint8_t {
//            Master,
//            Slave
//        };
//
//        explicit FloppyDisk(DriveType);
//
//    private:
//        // Floppy commands
//        enum class FloppyCommand : uint8_t {
//            ReadTrack = 0x02,
//            Specify = 0x03,
//            CheckStatus = 0x04,
//            WriteData = 0x05,
//            ReadData = 0x06,
//            Recalibrate = 0x07,
//            SenseInterrupt = 0x08,
//            WriteDeletedData = 0x09,
//            ReadDeletedData = 0x0C,
//            FormatTrack = 0x0D,
//            Seek = 0x0F,
//            Version = 0x10,
//            Verify = 0x16,
//        };
//
//    public:
//
//        // ^DiskDevice
//        int block_read(uint8_t *buffer, size_t start, size_t count) override;
//
//        int write_blocks(uint8_t *buffer, size_t start, size_t count) override;
//
////        // ^BlockDevice
////        virtual ssize_t read(FileDescription &, uint8_t *, ssize_t) override { return 0; }
////
////        virtual bool can_read(const FileDescription &) const override { return true; }
////
////        virtual ssize_t write(FileDescription &, const uint8_t *, ssize_t) override { return 0; }
////
////        virtual bool can_write(const FileDescription &) const override { return true; }
////
////        virtual const char *purpose() const override { return "Floppy Disk Controller"; }
//        void handle_irq(x86::intr_frame_t &intr_frame) override;
//
//    private:
//        // ^DiskDevice
////        virtual const char *class_name() const override;
//
//        // Helper functions
//        inline uint16_t lba2head(uint16_t lba) const {
//            return (lba % (SECTORS_PER_CYLINDER * 2)) / SECTORS_PER_CYLINDER;
//        } // Convert an LBA into a head value
//        inline uint16_t lba2cylinder(uint16_t lba) const {
//            return lba / (2 * SECTORS_PER_CYLINDER);
//        }                      // Convert an LBA into a cylinder value
//        inline uint16_t lba2sector(uint16_t lba) const {
//            return ((lba % SECTORS_PER_CYLINDER) + 1);
//        }                      // Convert an LBA into a sector value
//
//        void initialize();
//
//        bool read_sectors_with_dma(uint16_t, uint16_t, uint8_t *);
//
//        bool write_sectors_with_dma(uint16_t, uint16_t, const uint8_t *);
//
//        bool wait_for_irq() const;
//
//        bool is_busy() const;
//
//        bool seek(uint16_t);
//
//        bool recalibrate();
//
//        void send_byte(uint8_t) const;
//
//        void send_byte(FloppyCommand) const;
//
//        void write_dor(uint8_t) const;
//
//        void write_ccr(uint8_t) const;
//
//        void motor_enable(bool) const;
//
//        void configure_drive(uint8_t, uint8_t, uint8_t) const;
//
//        uint8_t read_byte() const;
//
//        uint8_t read_msr() const;
//
//        bool slave() const { return m_drive_type == DriveType::Slave; }
//
//        Mutex m_lock;
//        uint16_t m_io_base_addr{0};
//        volatile bool m_interrupted{false};
//
//        DriveType m_drive_type{DriveType::Master};
//        Frame *m_dma_buffer_page;
//        uint8_t m_controller_version{0};
//    };
//
//}

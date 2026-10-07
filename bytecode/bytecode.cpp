#include "..\main.h"

Bytecode::Bytecode(const std::string& filePath) : filePath(filePath) {}

Bytecode::~Bytecode() {
	close_file();
}

void Bytecode::operator()() {
	print_progress_bar();
	open_file();
	read_header();
	prototypesTotalSize = bytesUnread - 1;
	read_prototypes();
	close_file();
	erase_progress_bar();
}

void Bytecode::read_header() {
	read_file(5);
	assert(fileBuffer[0] == BC_HEADER[0] &&
		(
			(fileBuffer[1] == BC_HEADER[1] && (fileBuffer[2] == BC_HEADER[2]))
			|| (fileBuffer[1] == BC_HEADER_FS[1] && (fileBuffer[2] == BC_HEADER_FS[2]))
		),
		"Invalid header:\nExpected bytes " + byte_to_string(BC_HEADER[0]) + " " + byte_to_string(BC_HEADER[1]) + " " + byte_to_string(BC_HEADER[2])
		+ ", got " + byte_to_string(fileBuffer[0]) + " " + byte_to_string(fileBuffer[1]) + " " + byte_to_string(fileBuffer[2])
		+ "\n\nFile does not contain valid LuaJIT bytecode", filePath, DEBUG_INFO);
	header.version = fileBuffer[3];
	assert(header.version == BC_VERSION_1 || header.version == BC_VERSION_2 || header.version == BC_VERSION_3, "Invalid bytecode version (" + byte_to_string(fileBuffer[3]) + ")", filePath, DEBUG_INFO);
	header.flags = fileBuffer[4];
	assert(!(header.flags & ~(BC_F_BE | BC_F_STRIP | BC_F_FFI | (header.version == BC_VERSION_2 ? BC_F_FR2 : 0))), "Invalid flags (" + byte_to_string(header.flags) + ")", filePath, DEBUG_INFO);
	if (header.flags & BC_F_STRIP) return;

	const uint32_t chunknameSize = read_uleb128();
	read_file(chunknameSize);

	if (!fileBuffer.empty()) {
		header.chunkname.assign(
			reinterpret_cast<const char*>(fileBuffer.data()),
			fileBuffer.size()
		);
	} else {
		header.chunkname.clear();
	}
}

void Bytecode::read_prototypes() {
	std::vector<Prototype*> unlinkedPrototypes;

	while (buffer_next_block()) {
		assert(fileBuffer.size() >= MIN_PROTO_SIZE, "Prototype is too short", filePath, DEBUG_INFO);

		prototypes.emplace_back(*this);
		Prototype* proto = &prototypes.back();
		(*proto)(unlinkedPrototypes);

		print_progress_bar(prototypesTotalSize - bytesUnread - 1, prototypesTotalSize);
	}

	assert(unlinkedPrototypes.size() == 1, "Failed to link main prototype", filePath, DEBUG_INFO);
	main = unlinkedPrototypes.back();

	assert(
		(main->header.flags & BC_PROTO_VARARG)
		&& !main->header.parameters
		&& !main->upvalues.size(),
		"Main prototype has invalid header",
		filePath,
		DEBUG_INFO
	);
}

void Bytecode::open_file() {
	file = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	assert(file != INVALID_HANDLE_VALUE, "Unable to open file", filePath, DEBUG_INFO);

	LARGE_INTEGER size = {};
	assert(GetFileSizeEx(file, &size) != 0, "Failed to get file size", filePath, DEBUG_INFO);
	fileSize = static_cast<uint64_t>(size.QuadPart);
	assert(fileSize >= MIN_FILE_SIZE, "File is too small or empty", filePath, DEBUG_INFO);

	thread_local std::vector<uint8_t> t_fileBuffer;
	t_fileBuffer.resize(fileSize);

	DWORD bytesRead = 0;
	assert(ReadFile(file, t_fileBuffer.data(), static_cast<DWORD>(fileSize), &bytesRead, NULL) && bytesRead == fileSize, "Failed to read file", filePath, DEBUG_INFO);

	fileData = t_fileBuffer.data();
	bytesUnread = fileSize;
	fileCursor = 0;
	fileBuffer = std::span<const uint8_t>(fileData, fileSize);
}

void Bytecode::close_file() {
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
		file = INVALID_HANDLE_VALUE;
	}
}

void Bytecode::read_file(const uint32_t& byteCount) {
	assert(fileCursor + byteCount <= fileSize, "Read would exceed end of file", filePath, DEBUG_INFO);
	fileBuffer = std::span<const uint8_t>(fileData + fileCursor, byteCount);
	fileCursor += byteCount;
	bytesUnread -= byteCount;
}

uint32_t Bytecode::read_uleb128() {
    assert(fileCursor < fileSize, "Read would exceed end of file", filePath, DEBUG_INFO);
    uint32_t uleb128 = fileData[fileCursor++];
    bytesUnread--;
    
    if (uleb128 >= 0x80) {
        uleb128 &= 0x7F;
        uint8_t bitShift = 0;
        uint8_t b;
        
        do {
            bitShift += 7;
            assert(bitShift <= 28, "ULEB128 value is too large", filePath, DEBUG_INFO);
            assert(fileCursor < fileSize, "Read would exceed end of file", filePath, DEBUG_INFO);
            
            b = fileData[fileCursor++];
            bytesUnread--;
            uleb128 |= (uint32_t)(b & 0x7F) << bitShift;
            
        } while (b >= 0x80);
    }
    return uleb128;
}

bool Bytecode::buffer_next_block() {
	const uint32_t byteCount = read_uleb128();

	if (!byteCount) {
		assert(!bytesUnread, "Read unexpectedly reached end of file", filePath, DEBUG_INFO);
		return false;
	}

	read_file(byteCount);
	return true;
}

% d = fa_load(tse, mask [, type [,server]])
%
% Grab bpm data from the FA archiver
%
% Input:
%   tse = [start_time end_time]
%   mask = vector containing FA ids [1 2 3], or [78 79] etc.
%   type = 'F' for full data
%          'd' for 10072/64 decimated
%          'D' for 10072/16384 decimated
%          'C' for continuous data, in which case tse must be a single number
%              specifying the number of samples wanted
%          'CD' for continuous decimated data, and similarly tse must specify
%              number of samples
%          Use 'Z' suffix to select ID0 capture as well.
%   server = IP address of FA archiver
%
% Output:
%   d = object containing all of the data, containing the following fields:
%
%   d.decimation    Decimation factor corresponding to requested type
%   d.f_s           Sample rate of captured data at selected decimation
%   d.timestamp     Timestamp (in Matlab format) of first point
%   d.ids           Array of FA ids, copy of mask
%   d.data          The returned data
%   d.t             Timestamp array for each sample point
%   d.day           Matlab number of day containing first sample
%   d.id0           ID 0 values if Z option specified.
%
% FA ids are returned in the same order that they were requested, including
% any duplicates.

% Copyright (c) 2012 Michael Abbott, Diamond Light Source Ltd.
%
% This program is free software; you can redistribute it and/or modify
% it under the terms of the GNU General Public License as published by
% the Free Software Foundation; either version 2 of the License, or
% (at your option) any later version.
%
% This program is distributed in the hope that it will be useful,
% but WITHOUT ANY WARRANTY; without even the implied warranty of
% MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
% GNU General Public License for more details.
%
% You should have received a copy of the GNU General Public License
% along with this program; if not, write to the Free Software
% Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
%
% Contact:
%      Dr. Michael Abbott,
%      Diamond Light Source Ltd,
%      Diamond House,
%      Chilton,
%      Didcot,
%      Oxfordshire,
%      OX11 0DE
%      michael.abbott@diamond.ac.uk

function d = fa_load(tse, mask, type, server)
    % Assign defaults
    if ~exist('type', 'var')
        type = 'F';
    end
    if ~exist('server', 'var')  ||  strcmp(server, '')
        server = 'fa-archiver.diamond.ac.uk';
    elseif strcmp(server, 'booster')
        server = 'fa-archiver.diamond.ac.uk:8889';
    end

    % Parse arguments
    [decimation, frequency, typestr, ts_at_end, save_id0, max_id] = ...
        process_type(server, type);

    % Compute unique sorted list from id mask and remember the permuation.
    [request_mask, dummy, perm] = unique(mask);
    id_count = length(request_mask);

    % Prepare the request and send to server
    [request, tz_offset] = format_server_request( ...
        request_mask, max_id, save_id0, typestr, tse, decimation, ts_at_end);

    % Send formatted request to server.  This returns open connection to server
    % for reading response and returned datastream.
    [sc, cleanup] = send_request(server, request);                  %#ok<NASGU>

    % Parse response including initial header.
    [sample_count, block_size, initial_offset] = ...
        read_server_response(sc, typestr, tse);

    % Capture requested data.
    simple_data = strcmp(typestr, 'C')  ||  decimation == 1;
    [data, timestamps, durations, id_zeros, sample_count] = ...
        read_data(sc, sample_count, id_count, block_size, initial_offset, ...
            ts_at_end, save_id0, simple_data);

    % Prepare final result structure.  This involves some interpretation of the
    % captured timestamp information.
    d = format_results( ...
        decimation, frequency, request_mask, perm, ...
        data, timestamps, durations, id_zeros, ...
        tz_offset, save_id0, simple_data, sample_count, ...
        block_size, initial_offset);
end


% Process decimation request in light of server parameters.  This involves an
% initial parameter request to the server.
%   Note that the ts_at_end flag for DD data is an important optimisation;
% without this, data capture takes an unreasonably long time.
function [decimation, frequency, typestr, ts_at_end, save_id0, max_id] = ...
        process_type(server, type)

    % Read decimation and frequency parameters from server
    [sock, cleanup] = send_request(server, 'CdDFKC');               %#ok<NASGU>
    params = textscan(read_string(sock), '%f');
    first_dec  = params{1}(1);
    second_dec = params{1}(2);
    frequency  = params{1}(3);
    max_id     = params{1}(4);
    stream_dec = params{1}(5);

    % Parse request
    save_id0 = type(end) == 'Z';
    if save_id0; type = type(1:end-1); end

    ts_at_end = false;
    if strcmp(type, 'F') || strcmp(type, 'C')
        decimation = 1;
        typestr = type;
    elseif strcmp(type, 'CD')
        if stream_dec == 0; error('No decimated data from server'); end
        decimation = stream_dec;
        typestr = 'C';
    elseif strcmp(type, 'd')
        decimation = first_dec;
        typestr = 'D';
    elseif strcmp(type, 'D')
        decimation = first_dec * second_dec;
        typestr = 'DD';
        ts_at_end = true;
    else
        error('Invalid datatype requested');
    end
    frequency = frequency / decimation;
end


function [request, tz_offset] = format_server_request( ...
        request_mask, max_id, save_id0, typestr, tse, decimation, ts_at_end)

    maskstr = format_mask(request_mask, max_id);
    if save_id0; id0_req = 'Z'; else id0_req = ''; end
    if typestr == 'C'
        % Continuous data request
        tz_offset = get_tz_offset(now);
        request = sprintf('S%sTE%s', maskstr, id0_req);
        if decimation > 1; request = [request 'D']; end
    else
        tz_offset = get_tz_offset(tse(2));
        if ts_at_end; ts_req = 'A'; else ts_req = 'E'; end
        request = sprintf('R%sM%sT%sZET%sZNAT%s%s', typestr, maskstr, ...
            format_time(tse(1) - tz_offset), ...
            format_time(tse(2) - tz_offset), ts_req, id0_req);
    end
end


% Reads initial response from server including timestamp header, raises error if
% server rejects request.
function [sample_count, block_size, initial_offset] = ...
        read_server_response(sc, typestr, tse)

    % Check the error code response and raise an error if failed
    buf = read_bytes(sc, 1, true);
    rc = buf.get();
    if rc ~= 0
        % On error the entire response is the error message.
        error([char(rc) read_string(sc)]);
    end

    if typestr == 'C'
        % For continuous data tse is the count.
        sample_count = tse(1);
    else
        % For historical data get the sample count at the head of the response.
        sample_count = read_long_array(sc, 1);
    end

    % Read the timestamp header with block size and initial offset.
    header = read_int_array(sc, 2);
    block_size = header(1);             % Number of samples per block
    initial_offset = header(2);         % Offset into first block read
end


% Prepares arrays and flags for reading data from server.
function [ ...
        field_count, block_offset, read_block_size, ...
        data, timestamps, durations, id_zeros] = ...
    prepare_data( ...
        sample_count, id_count, simple_data, initial_offset, block_size, ...
        ts_at_end, save_id0)

    if simple_data
        field_count = 1;
        data = zeros(2, id_count, sample_count);
    else
        field_count = 4;
        data = zeros(2, 4, id_count, sample_count);
    end

    % Prepare for reading.  Alas we need to support both options for timestamps
    % if we want to support C data, as C data cannot be delivered with
    % timestamps at end.
    if ts_at_end
        block_offset = 0;
        read_block_size = 65536;
        % Need dummy values for values read at end
        timestamps = 0;
        durations = 0;
        id_zeros = 0;
    else
        block_offset = initial_offset;
        read_block_size = block_size;

        % Prepare timestamps buffers, we guess a sensible initial size
        timestamps = zeros(round(sample_count / block_size) + 2, 1);
        durations  = zeros(round(sample_count / block_size) + 2, 1);
        if save_id0
            id_zeros = int32(zeros(round(sample_count / block_size) + 2, 1));
        else
            id_zeros = 0;   % Dummy value
        end
    end
end


% Reads data from server.
function [data, timestamps, durations, id_zeros, sample_count] = ...
    read_data( ...
        sc, sample_count, id_count, block_size, initial_offset, ...
        ts_at_end, save_id0, simple_data)

    function truncate_data()
        warning('Data truncated');
        sample_count = samples_read;
        if simple_data
            data = data(:, :, 1:samples_read);
        else
            data = data(:, :, :, 1:samples_read);
        end
    end

    % Reads one block of timestamp, returns false if early end of data
    % encountered.  Normal case when timestamps are interleaved with the data
    % stream.
    function ok = read_timestamp_block()
        ok = true;
        try
            timestamp_buf = read_bytes(sc, ts_buf_size, true);
        catch me
            if ~strcmp(me.identifier, 'fa_load:read_bytes')
                rethrow(me)
            end

            % If we suffer from FA archiver buffer underrun it will be the
            % timestamp buffer that fails to be read.  If this occurs,
            % truncate sample_count and break -- we'll return what we have
            % in hand.
            ok = false;
            return
        end
        timestamps(ts_read + 1) = timestamp_buf.getLong();
        durations (ts_read + 1) = timestamp_buf.getInt();
        if save_id0
            id_zeros(ts_read + 1) = timestamp_buf.getInt();
        end
        ts_read = ts_read + 1;
    end


    % Reads timestamp block at end of data.  Special case for DD data when
    % interleaved timestamps slow us down too much.
    function read_timestamp_at_end()
        % Timestamps at end are sent as a count followed by all the timestamps
        % together followed by all the durations together.
        ts_read = read_int_array(sc, 1);
        timestamps = read_long_array(sc, ts_read);
        durations  = read_int_array(sc, ts_read);
        if save_id0
            id_zeros = read_int_array(sc, ts_read);
        end
    end


    % Reads data and converts to target format
    function read_data_block()
        % Work out how many samples in the next block
        block_count = read_block_size - block_offset;
        if samples_read + block_count > sample_count
            block_count = sample_count - samples_read;
        end

        % Read the data and convert to matlab format.
        int_buf = read_int_array(sc, 2 * field_count * id_count * block_count);
        if simple_data
            data(:, :, samples_read + 1:samples_read + block_count) = ...
                reshape(int_buf, 2, id_count, block_count);
        else
            data(:, :, :, samples_read + 1:samples_read + block_count) = ...
                reshape(int_buf, 2, 4, id_count, block_count);
        end

        block_offset = 0;
        samples_read = samples_read + block_count;
    end


    % Support for cancellable waiting
    function [wh, cleanup] = create_waitbar(title)
        wh = waitbar(0, 'Fetching data', ...
            'CreateCancelBtn', 'setappdata(gcbf,''cancelling'',1)');
        cleanup = onCleanup(@() delete(wh));
        setappdata(wh, 'cancelling', 0);
    end

    function ok = advance_waitbar(fraction)
        waitbar(fraction, wh);
        ok = ~getappdata(wh, 'cancelling');
    end


    [field_count, block_offset, read_block_size, ...
     data, timestamps, durations, id_zeros] = prepare_data( ...
        sample_count, id_count, simple_data, initial_offset, block_size, ...
        ts_at_end, save_id0);
    if save_id0; ts_buf_size = 16; else ts_buf_size = 12; end

    % If possible create the wait bar
    if ~ts_at_end
        [wh, cleanup] = create_waitbar('Fetching data');
    end

    % Read the requested data block by block
    samples_read = 0;
    ts_read = 0;
    while samples_read < sample_count
        if ~ts_at_end
            % Advance the progress bar and read the next timestamp block.  If
            % either of these fails then truncate the data and we're done.
            if ~advance_waitbar(samples_read / sample_count)  ||  ...
               ~read_timestamp_block()
                truncate_data();
                break
            end
        end

        read_data_block();
    end

    if ts_at_end
        read_timestamp_at_end();
    end
end


% Prepares final result.
function d = format_results( ...
        decimation, frequency, request_mask, perm, ...
        data, timestamps, durations, id_zeros, ...
        tz_offset, save_id0, simple_data, sample_count, ...
        block_size, initial_offset)

    % Prepare the result data structure
    d = struct();
    d.decimation = decimation;
    d.f_s = frequency;

    [d.day, d.timestamp, d.t] = process_timestamps( ...
        timestamps, durations, ...
        tz_offset, sample_count, block_size, initial_offset);
    if save_id0
        d.id0 = process_id0( ...
            id_zeros, sample_count, block_size, initial_offset, decimation);
    end

    % Restore the originally requested permutation if necessary.
    if any(diff(perm) ~= 1)
        d.ids = request_mask(perm);
        if simple_data
            d.data = data(:, perm, :);
        else
            d.data = data(:, :, perm, :);
        end
    else
        d.ids = request_mask;
        d.data = data;
    end
end


% Formats request mask into a format suitable for sending to the server.
function result = format_mask(mask, max_id)
    % Validate request
    if numel(mask) == 0
        error('Empty list of ids');
    end
    if mask(1) < 0 || max_id <= mask(end)
        error('Invalid range of ids');
    end

    % Assemble array of ints from ids and send as raw mask array
    mask_array = zeros(1, max_id / 32);
    for id = mask
        ix = floor(id / 32) + 1;
        mask_array(ix) = bitor(mask_array(ix), bitshift(1, mod(id, 32)));
    end
    result = ['R' sprintf('%08X', mask_array(end:-1:1))];
end


% Opens socket channel to given server and sends request.  Both the opened
% socket and a cleanup handler are returned, the socket will be closed when
% cleanup is discarded.
function [channel, cleanup] = send_request(server, request)
    import java.nio.channels.SocketChannel;
    import java.net.InetSocketAddress;
    import java.lang.String;
    import java.nio.ByteBuffer;

    % Allow for a non standard port to be specified as part of the server name.
    [server, port] = strtok(server, ':');
    if port; port = str2double(port(2:end)); else port = 8888; end

    % Open the channel and connect to the server.
    channel = SocketChannel.open();
    channel.connect(InetSocketAddress(server, port));

    % Ensure that the socket is closed when no longer needed.
    socket = channel.socket();
    cleanup = onCleanup(@() socket.close());

    % Send request with newline termination.
    request = ByteBuffer.wrap(String([request 10]).getBytes('US-ASCII'));
    channel.write(request);
end


% Reads a block of the given number of bytes from the socket
function [buf, pos] = read_bytes(sc, count, require)
    import java.nio.ByteBuffer;
    import java.nio.ByteOrder;

    buf = ByteBuffer.allocate(count);
    buf.order(ByteOrder.LITTLE_ENDIAN);
    while buf.remaining() ~= 0
        if sc.read(buf) < 0
            break
        end
    end
    pos = buf.position();
    if require && pos ~= count
        throw(MException( ...
            'fa_load:read_bytes', 'Too few bytes received from server'))
    end
    buf.flip();
end

function a = read_int_array(sc, count)
    import java.nio.IntBuffer;

    buf = read_bytes(sc, 4 * count, true);
    ints = IntBuffer.allocate(count);
    ints.put(buf.asIntBuffer());
    a = double(ints.array());
end

function a = read_long_array(sc, count)
    import java.nio.LongBuffer;

    buf = read_bytes(sc, 8 * count, true);
    longs = LongBuffer.allocate(count);
    longs.put(buf.asLongBuffer());
    a = double(longs.array());
end

function s = read_string(sc)
    [buf, len] = read_bytes(sc, 4096, false);
    bytes = buf.array();
    s = char(bytes(1:len))';
end


% Converts matlab timestamp into ISO 8601 format as expected by FA server.
function str = format_time(time)
    format = 'yyyy-mm-ddTHH:MM:SS';
    str = datestr(time, format);
    % Work out the remaining unformatted fraction of a second
    delta_s = 3600 * 24 * (time - datenum(str, format));
    if delta_s > 0.9999; delta_s = 0.9999; end      % Fudge for last fraction
    if 0 < delta_s
        nano = sprintf('%.4f', delta_s);
        str = [str nano(2:end)];
    end
end


% Returns the offset to be subtracted from matlab time to get UTC time.
function tz_offset = get_tz_offset(time)
    import java.util.TimeZone;

    % Convert the time to UTC by subtracting the time zone specific offset.
    % Unfortunately getting the correct daylight saving offset is a bit of a
    % stab in the dark as we should be asking in local time.
    scaling = 1 / (1e3 * 3600 * 24);    % Java time in milliseconds
    epoch = 719529;                     % 1970-01-01 in matlab time
    local_tz = TimeZone.getDefault();
    java_time = (time - epoch) / scaling - local_tz.getRawOffset();
    tz_offset = local_tz.getOffset(java_time) * scaling;
end


% Computes timebase from timestamps and offsets.
% Note that we could avoid converting timestamps to doubles until subtracting
% ts_offset below, but in fact we have just enough precision for microseconds.
function [day, start_time, ts] = process_timestamps( ...
        timestamps, durations, ...
        tz_offset, sample_count, block_size, initial_offset)

    scaling = 1 / (1e6 * 3600 * 24);    % Archiver time in microseconds
    epoch = 719529;                     % 1970-01-01 in matlab time
    start_time = timestamps(1) * scaling + epoch + tz_offset;
    day = floor(start_time);

    ts_offset = (day - epoch - tz_offset) / scaling;
    timestamps = scaling * (timestamps - ts_offset);
    durations = (scaling * durations) * (0 : block_size - 1) / block_size;

    ts = repmat(timestamps, 1, block_size) + durations;
    ts = reshape(ts', [], 1);
    ts = ts(initial_offset + 1:initial_offset + sample_count);
end


% Computes id0 array from captured information.
function id0 = process_id0( ...
        id_zeros, sample_count, block_size, initial_offset, decimation)
    id_zeros = int32(id_zeros);
    offsets = int32(decimation * (0 : block_size - 1));
    id0 = repmat(id_zeros, 1, block_size) + repmat(offsets, size(id_zeros), 1);
    id0 = reshape(id0', [], 1);
    id0 = id0(initial_offset + 1:initial_offset + sample_count);
end

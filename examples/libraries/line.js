function error(message) {
    print("[ERROR] " + message);
}

// PolyLine
var LINE_DIMENSIONS = { x: 2000, y: 2000, z: 2000 };
var MAX_LINE_LENGTH = 40; // This must be 2 or greater;
var PolyLine = function(position, color, defaultStrokeWidth) {
    this.position = position;
    this.color = color;
    this.defaultStrokeWidth = defaultStrokeWidth;
    this.points = [
    ];
    this.strokeWidths = [
    ];
    this.normals = [
    ]
    this.entityID = Entities.addEntity({
        type: "PolyLine",
        position: position,
        linePoints: this.points,
        normals: this.normals,
        strokeWidths: this.strokeWidths,
        dimensions: LINE_DIMENSIONS,
        color: color,
        lifetime: 20
    });
};

PolyLine.prototype.enqueuePoint = function(position) {
    if (this.isFull()) {
        error("Hit max PolyLine size");
        return;
    }

    position = Vec3.subtract(position, this.position);
    this.points.push(position);
    this.normals.push({ x: 1, y: 0, z: 0 });
    this.strokeWidths.push(this.defaultStrokeWidth);
    Entities.editEntity(this.entityID, {
        linePoints: this.points,
        normals: this.normals,
        strokeWidths: this.strokeWidths
    });
};

PolyLine.prototype.dequeuePoint = function() {
    if (this.points.length == 0) {
        error("Hit min PolyLine size");
        return;
    }

    this.points = this.points.slice(1);
    this.normals = this.normals.slice(1);
    this.strokeWidths = this.strokeWidths.slice(1);

    Entities.editEntity(this.entityID, {
        linePoints: this.points,
        normals: this.normals,
        strokeWidths: this.strokeWidths
    });
};

PolyLine.prototype.getFirstPoint = function() {
    return Vec3.sum(this.position, this.points[0]);
};

PolyLine.prototype.getLastPoint = function() {
    return Vec3.sum(this.position, this.points[this.points.length - 1]);
};

PolyLine.prototype.getSize = function() {
    return this.points.length;
}

PolyLine.prototype.isFull = function() {
    return this.points.length >= MAX_LINE_LENGTH;
};

PolyLine.prototype.destroy = function() {
    Entities.deleteEntity(this.entityID);
    this.points = [];
};



// InfiniteLine
InfiniteLine = function(position, color) {
    this.position = position;
    this.color = color;
    this.lines = [new PolyLine(position, color, 0.01)];
    this.size = 0;
};

InfiniteLine.prototype.enqueuePoint = function(position) {
    var currentLine;

    if (this.lines.length == 0) {
        currentLine = new PolyLine(position, this.color, 0.01);
        this.lines.push(currentLine);
    } else {
        currentLine = this.lines[this.lines.length - 1];
    }

    if (currentLine.isFull()) {
        var newLine = new PolyLine(currentLine.getLastPoint(), this.color, 0.01);
        newLine.enqueuePoint(currentLine.getLastPoint());
        this.lines.push(newLine);
        currentLine = newLine;
    }

    currentLine.enqueuePoint(position);

    ++this.size;
};

InfiniteLine.prototype.dequeuePoint = function() {
    if (this.lines.length == 0) {
        error("Trying to dequeue from InfiniteLine when no points are left");
        return;
    }

    var lastLine = this.lines[0];
    lastLine.dequeuePoint();

    if (lastLine.getSize() <= 1) {
        this.lines = this.lines.slice(1);
    }

    --this.size;
};

InfiniteLine.prototype.getFirstPoint = function() {
    return this.lines.length > 0 ? this.lines[0].getFirstPoint() : null;
};

InfiniteLine.prototype.getLastPoint = function() {
    return this.lines.length > 0 ? this.lines[lines.length - 1].getLastPoint() : null;
};

InfiniteLine.prototype.destroy = function() {
    for (var i = 0; i < this.lines.length; ++i) {
        this.lines[i].destroy();
    }

    this.size = 0;
};
